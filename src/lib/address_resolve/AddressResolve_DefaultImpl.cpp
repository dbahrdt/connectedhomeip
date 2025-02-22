/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <lib/address_resolve/AddressResolve_DefaultImpl.h>

namespace chip {
namespace AddressResolve {
namespace Impl {
namespace {

static constexpr System::Clock::Timeout kInvalidTimeout{ System::Clock::Timeout::max() };

// IP addess "suitability"
//   - Larger value means "more suitable"
//   - Enum ordered ascending for easier read. Note however that order of
//     checks MUST match in ScoreIpAddress below.
enum class IpScore : unsigned
{
    kInvalid = 0, // No address available

    // "Other" IPv6 include:
    //   - invalid addresses (have seen router bugs during interop testing)
    //   - embedded IPv4 (::/80)
    kOtherIpv6                     = 1,
    kIpv4                          = 2, // Not Matter SPEC, so low priority
    kLinkLocal                     = 3, // Valid only on an interface
    kUniqueLocal                   = 4, // ULA. Thread devices use this
    kGlobalUnicast                 = 5, // Maybe routable, not local subnet
    kUniqueLocalWithSharedPrefix   = 6, // Prefix seems to match a local interface
    kGlobalUnicastWithSharedPrefix = 7, // Prefix seems to match a local interface
};

constexpr unsigned ScoreValue(IpScore score)
{
    return static_cast<unsigned>(score);
}

/**
 * Gives a score for an IP address, generally related to "how good" the address
 * is and how likely it is for it to be reachable.
 */
IpScore ScoreIpAddress(const Inet::IPAddress & ip, Inet::InterfaceId interfaceId)
{
    if (ip.IsIPv6())
    {
        if (interfaceId.MatchLocalIPv6Subnet(ip))
        {
            if (ip.IsIPv6GlobalUnicast())
            {
                return IpScore::kGlobalUnicastWithSharedPrefix;
            }
            else if (ip.IsIPv6ULA())
            {
                return IpScore::kUniqueLocalWithSharedPrefix;
            }
        }
        if (ip.IsIPv6GlobalUnicast())
        {
            return IpScore::kGlobalUnicast;
        }

        if (ip.IsIPv6ULA())
        {
            return IpScore::kUniqueLocal;
        }

        if (ip.IsIPv6LinkLocal())
        {
            return IpScore::kLinkLocal;
        }

        return IpScore::kOtherIpv6;
    }
    else
    {
        return IpScore::kIpv4;
    }
}

} // namespace

void NodeLookupHandle::ResetForLookup(System::Clock::Timestamp now, const NodeLookupRequest & request)
{
    mRequestStartTime = now;
    mRequest          = request;
    mBestResult       = ResolveResult();
    mBestAddressScore = ScoreValue(IpScore::kInvalid);
}

void NodeLookupHandle::LookupResult(const ResolveResult & result)
{
#if CHIP_PROGRESS_LOGGING
    char addr_string[Transport::PeerAddress::kMaxToStringSize];
    result.address.ToString(addr_string);
#endif

    unsigned newScore = ScoreValue(ScoreIpAddress(result.address.GetIPAddress(), result.address.GetInterface()));
    if (newScore > mBestAddressScore)
    {
        mBestResult       = result;
        mBestAddressScore = newScore;

        if (!mBestResult.address.GetIPAddress().IsIPv6LinkLocal())
        {
            // Only use the DNS-SD resolution's InterfaceID for addresses that are IPv6 LLA.
            // For all other addresses, we should rely on the device's routing table to route messages sent.
            // Forcing messages down an InterfaceId might fail. For example, in bridged networks like Thread,
            // mDNS advertisements are not usually received on the same interface the peer is reachable on.
            mBestResult.address.SetInterface(Inet::InterfaceId::Null());
            ChipLogDetail(Discovery, "Lookup clearing interface for non LL address");
        }

#if CHIP_PROGRESS_LOGGING
        ChipLogProgress(Discovery, "%s: new best score: %u", addr_string, mBestAddressScore);
    }
    else
    {
        ChipLogProgress(Discovery, "%s: score has not improved: %u", addr_string, newScore);
#endif
    }
}

System::Clock::Timeout NodeLookupHandle::NextEventTimeout(System::Clock::Timestamp now)
{
    const System::Clock::Timestamp elapsed = now - mRequestStartTime;

    if (elapsed < mRequest.GetMinLookupTime())
    {
        return mRequest.GetMinLookupTime() - elapsed;
    }
    else if (elapsed < mRequest.GetMaxLookupTime())
    {
        return mRequest.GetMaxLookupTime() - elapsed;
    }
    else
    {
        ChipLogError(Discovery, "Unexpected timeout: lookup should have been cleaned already.");
        return System::Clock::Timeout::zero();
    }
}

NodeLookupAction NodeLookupHandle::NextAction(System::Clock::Timestamp now)
{
    const System::Clock::Timestamp elapsed = now - mRequestStartTime;

    ChipLogProgress(Discovery, "Checking node lookup status after %lu ms", static_cast<unsigned long>(elapsed.count()));

    // We are still within the minimal search time. Wait for more results.
    if (elapsed < mRequest.GetMinLookupTime())
    {
        ChipLogProgress(Discovery, "Keeping DNSSD lookup active");
        return NodeLookupAction::KeepSearching();
    }

    // Minimal time to search reached. If any IP available, ready to return it.
    if (mBestAddressScore > ScoreValue(IpScore::kInvalid))
    {
        return NodeLookupAction::Success(mBestResult);
    }

    // Give up if the maximum search time has been reached
    if (elapsed >= mRequest.GetMaxLookupTime())
    {
        return NodeLookupAction::Error(CHIP_ERROR_TIMEOUT);
    }

    return NodeLookupAction::KeepSearching();
}

CHIP_ERROR Resolver::LookupNode(const NodeLookupRequest & request, Impl::NodeLookupHandle & handle)
{
    handle.ResetForLookup(mTimeSource.GetMonotonicTimestamp(), request);
    ReturnErrorOnFailure(Dnssd::Resolver::Instance().ResolveNodeId(request.GetPeerId(), Inet::IPAddressType::kAny));
    mActiveLookups.PushBack(&handle);
    ReArmTimer();
    return CHIP_NO_ERROR;
}

CHIP_ERROR Resolver::Init(System::Layer * systemLayer)
{
    mSystemLayer = systemLayer;
    Dnssd::Resolver::Instance().SetOperationalDelegate(this);
    return CHIP_NO_ERROR;
}

void Resolver::OnOperationalNodeResolved(const Dnssd::ResolvedNodeData & nodeData)
{
    auto it = mActiveLookups.begin();
    while (it != mActiveLookups.end())
    {
        auto current = it;
        it++;
        if (current->GetRequest().GetPeerId() != nodeData.mPeerId)
        {
            continue;
        }

        ResolveResult result;

        result.address.SetPort(nodeData.mPort);
        result.address.SetInterface(nodeData.mInterfaceId);
        result.mrpConfig   = nodeData.GetMRPConfig();
        result.supportsTcp = nodeData.mSupportsTcp;

        for (size_t i = 0; i < nodeData.mNumIPs; i++)
        {
#if !INET_CONFIG_ENABLE_IPV4
            if (!nodeData.mAddress[i].IsIPv6())
            {
                ChipLogError(Discovery, "Skipping IPv4 address during operational resolve.");
                continue;
            }
#endif
            result.address.SetIPAddress(nodeData.mAddress[i]);
            current->LookupResult(result);
        }

        HandleAction(current);
    }

    ReArmTimer();
}

void Resolver::HandleAction(IntrusiveList<NodeLookupHandle>::Iterator & current)
{
    const NodeLookupAction action = current->NextAction(mTimeSource.GetMonotonicTimestamp());

    if (action.Type() == NodeLookupResult::kKeepSearching)
    {
        // No change in iterator
        return;
    }

    // final result, handle either success or failure
    const PeerId peerId     = current->GetRequest().GetPeerId();
    NodeListener * listener = current->GetListener();
    mActiveLookups.Erase(current);

    // ensure action is taken AFTER the current current lookup is marked complete
    // This allows failure handlers to deallocate structures that may
    // contain the active lookup data as a member (intrusive lists members)
    switch (action.Type())
    {
    case NodeLookupResult::kLookupError:
        listener->OnNodeAddressResolutionFailed(peerId, action.ErrorResult());
        break;
    case NodeLookupResult::kLookupSuccess:
        listener->OnNodeAddressResolved(peerId, action.ResolveResult());
        break;
    default:
        ChipLogError(Discovery, "Unexpected lookup state (not success or fail).");
        break;
    }
}

void Resolver::HandleTimer()
{
    auto it = mActiveLookups.begin();
    while (it != mActiveLookups.end())
    {
        auto current = it;
        it++;

        HandleAction(current);
    }

    ReArmTimer();
}

void Resolver::OnOperationalNodeResolutionFailed(const PeerId & peerId, CHIP_ERROR error)
{
    auto it = mActiveLookups.begin();
    while (it != mActiveLookups.end())
    {
        auto current = it;
        it++;
        if (current->GetRequest().GetPeerId() != peerId)
        {
            continue;
        }

        NodeListener * listener = current->GetListener();
        mActiveLookups.Erase(current);

        // Failure callback only called after iterator was cleared:
        // This allows failure handlers to deallocate structures that may
        // contain the active lookup data as a member (intrusive lists members)
        listener->OnNodeAddressResolutionFailed(peerId, error);
    }
    ReArmTimer();
}

void Resolver::ReArmTimer()
{
    mSystemLayer->CancelTimer(&OnResolveTimer, static_cast<void *>(this));

    System::Clock::Timestamp now = mTimeSource.GetMonotonicTimestamp();

    System::Clock::Timeout nextTimeout = kInvalidTimeout;
    for (auto it = mActiveLookups.begin(); it != mActiveLookups.end(); it++)
    {
        System::Clock::Timeout timeout = it->NextEventTimeout(now);

        if (timeout < nextTimeout)
        {
            nextTimeout = timeout;
        }
    }

    if (nextTimeout == kInvalidTimeout)
    {
        // Generally this is only expected when no active lookups exist
        ChipLogProgress(Discovery, "Discovery does not require any more timeouts");
        return;
    }

    CHIP_ERROR err = mSystemLayer->StartTimer(nextTimeout, &OnResolveTimer, static_cast<void *>(this));
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Discovery, "Timer schedule error %s assumed permanent", err.AsString());

        // Clear out all active lookups: without timers there is no guarantee of success
        auto it = mActiveLookups.begin();
        while (it != mActiveLookups.end())
        {
            const PeerId peerId     = it->GetRequest().GetPeerId();
            NodeListener * listener = it->GetListener();

            mActiveLookups.Erase(it);
            it = mActiveLookups.begin();

            // Callback only called after active lookup is cleared
            // This allows failure handlers to deallocate structures that may
            // contain the active lookup data as a member (intrusive lists members)
            listener->OnNodeAddressResolutionFailed(peerId, err);
        }
    }
}

} // namespace Impl

Resolver & Resolver::Instance()
{
    static Impl::Resolver gResolver;
    return gResolver;
}

} // namespace AddressResolve
} // namespace chip
