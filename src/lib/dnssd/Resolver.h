/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

#pragma once

#include <cstdint>
#include <limits>
#include <utility>

#include "lib/support/logging/CHIPLogging.h"
#include <inet/IPAddress.h>
#include <inet/InetInterface.h>
#include <inet/UDPEndPoint.h>
#include <lib/core/CHIPError.h>
#include <lib/core/Optional.h>
#include <lib/core/PeerId.h>
#include <lib/dnssd/Constants.h>
#include <lib/support/BytesToHex.h>
#include <messaging/ReliableMessageProtocolConfig.h>

namespace chip {
namespace Dnssd {

struct ResolvedNodeData
{
    // TODO: use pool to allow dynamic
    static constexpr unsigned kMaxIPAddresses = 5;

    static bool IsIpLess(const Inet::IPAddress & a, const Inet::IPAddress & b)
    {
        // Link-local last
        if (a.IsIPv6LinkLocal() && !b.IsIPv6LinkLocal())
        {
            return false;
        }
        if (!a.IsIPv6LinkLocal() && b.IsIPv6LinkLocal())
        {
            return true;
        }

        // IPv6 before IPv4
        if (a.IsIPv6() && !b.IsIPv6())
        {
            return false;
        }
        if (!a.IsIPv6() && b.IsIPv6())
        {
            return true;
        }

        // no ordering, do not care
        return false;
    }

    void LogNodeIdResolved() const
    {
#if CHIP_PROGRESS_LOGGING
        char addrBuffer[Inet::IPAddress::kMaxStringLength];

        // Would be nice to log the interface id, but sorting out how to do so
        // across our differnet InterfaceId implementations is a pain.
        ChipLogProgress(Discovery, "Node ID resolved for 0x" ChipLogFormatX64, ChipLogValueX64(mPeerId.GetNodeId()));
        for (unsigned i = 0; i < mNumIPs; ++i)
        {
            mAddress[i].ToString(addrBuffer);
            ChipLogProgress(Discovery, "    Addr %u: [%s]:%" PRIu16, i, addrBuffer, mPort);
        }
#endif // CHIP_PROGRESS_LOGGING
    }

    /// Sorts IP addresses in a consistent order. Specifically places
    /// Link-local IPv6 addresses at the end (e.g. mDNS reflector services in Unify will
    /// return link-local addresses that will not work) and prioritizes global IPv6 addresses
    /// before IPv4 ones.
    void PrioritizeAddresses()
    {
        // Slow sort, however we have maximum kMaxIPAddreses, so this is good enough for now
        for (unsigned i = 0; i + 1 < mNumIPs; i++)
        {
            for (unsigned j = i + 1; i < mNumIPs; i++)
            {
                if (IsIpLess(mAddress[j], mAddress[i]))
                {
                    std::swap(mAddress[i], mAddress[j]);
                }
            }
        }
    }

    ReliableMessageProtocolConfig GetMRPConfig() const
    {
        const ReliableMessageProtocolConfig defaultConfig = GetLocalMRPConfig();
        return ReliableMessageProtocolConfig(GetMrpRetryIntervalIdle().ValueOr(defaultConfig.mIdleRetransTimeout),
                                             GetMrpRetryIntervalActive().ValueOr(defaultConfig.mActiveRetransTimeout));
    }
    Optional<System::Clock::Milliseconds32> GetMrpRetryIntervalIdle() const { return mMrpRetryIntervalIdle; }
    Optional<System::Clock::Milliseconds32> GetMrpRetryIntervalActive() const { return mMrpRetryIntervalActive; }

    bool IsDeviceTreatedAsSleepy(const ReliableMessageProtocolConfig * defaultMRPConfig) const
    {
        // If either retry interval (Idle - CRI, Active - CRA) has a value and that value is greater
        // than the value passed to this function, then the peer device will be treated as if it is
        // a Sleepy End Device (SED)
        if ((mMrpRetryIntervalIdle.HasValue() && (mMrpRetryIntervalIdle.Value() > defaultMRPConfig->mIdleRetransTimeout)) ||
            (mMrpRetryIntervalActive.HasValue() && (mMrpRetryIntervalActive.Value() > defaultMRPConfig->mActiveRetransTimeout)))
        {
            return true;
        }
        return false;
    }

    PeerId mPeerId;
    size_t mNumIPs = 0;
    Inet::InterfaceId mInterfaceId;
    Inet::IPAddress mAddress[kMaxIPAddresses];
    uint16_t mPort                         = 0;
    char mHostName[kHostNameMaxLength + 1] = {};
    bool mSupportsTcp                      = false;
    Optional<System::Clock::Milliseconds32> mMrpRetryIntervalIdle;
    Optional<System::Clock::Milliseconds32> mMrpRetryIntervalActive;
    System::Clock::Timestamp mExpiryTime;
};

constexpr size_t kMaxDeviceNameLen         = 32;
constexpr size_t kMaxRotatingIdLen         = 50;
constexpr size_t kMaxPairingInstructionLen = 128;

struct DiscoveredNodeData
{
    // TODO(cecille): is 4 OK? IPv6 LL, GUA, ULA, IPv4?
    static constexpr unsigned kMaxIPAddresses = 5;
    char hostName[kHostNameMaxLength + 1];
    char instanceName[Commission::kInstanceNameMaxLength + 1];
    uint16_t longDiscriminator;
    uint16_t vendorId;
    uint16_t productId;
    uint8_t commissioningMode;
    // TODO: possibly 32-bit - see spec issue #3226
    uint16_t deviceType;
    char deviceName[kMaxDeviceNameLen + 1];
    uint8_t rotatingId[kMaxRotatingIdLen];
    size_t rotatingIdLen;
    uint16_t pairingHint;
    char pairingInstruction[kMaxPairingInstructionLen + 1];
    bool supportsTcp;
    Optional<System::Clock::Milliseconds32> mrpRetryIntervalIdle;
    Optional<System::Clock::Milliseconds32> mrpRetryIntervalActive;
    uint16_t port;
    unsigned numIPs;
    Inet::InterfaceId interfaceId; // interface ID for the packet processed
    Inet::IPAddress ipAddress[kMaxIPAddresses];

    void Reset()
    {
        memset(hostName, 0, sizeof(hostName));
        memset(instanceName, 0, sizeof(instanceName));
        longDiscriminator = 0;
        vendorId          = 0;
        productId         = 0;
        commissioningMode = 0;
        deviceType        = 0;
        memset(deviceName, 0, sizeof(deviceName));
        memset(rotatingId, 0, sizeof(rotatingId));
        rotatingIdLen = 0;
        memset(pairingInstruction, 0, sizeof(pairingInstruction));
        pairingHint            = 0;
        supportsTcp            = false;
        mrpRetryIntervalIdle   = NullOptional;
        mrpRetryIntervalActive = NullOptional;
        numIPs                 = 0;
        port                   = 0;
        for (unsigned i = 0; i < kMaxIPAddresses; ++i)
        {
            ipAddress[i] = chip::Inet::IPAddress::Any;
        }
    }
    DiscoveredNodeData() { Reset(); }
    bool IsHost(const char * host) const { return strcmp(host, hostName) == 0; }
    bool IsInstanceName(const char * instance) const { return strcmp(instance, instanceName) == 0; }
    bool IsValid() const { return !IsHost("") && ipAddress[0] != chip::Inet::IPAddress::Any; }
    Optional<System::Clock::Milliseconds32> GetMrpRetryIntervalIdle() const { return mrpRetryIntervalIdle; }
    Optional<System::Clock::Milliseconds32> GetMrpRetryIntervalActive() const { return mrpRetryIntervalActive; }

    bool IsDeviceTreatedAsSleepy(const ReliableMessageProtocolConfig * defaultMRPConfig) const
    {
        // If either retry interval (Idle - CRI, Active - CRA) has a value and that value is greater
        // than the value passed to this function, then the peer device will be treated as if it is
        // a Sleepy End Device (SED)
        if ((mrpRetryIntervalIdle.HasValue() && (mrpRetryIntervalIdle.Value() > defaultMRPConfig->mIdleRetransTimeout)) ||
            (mrpRetryIntervalActive.HasValue() && (mrpRetryIntervalActive.Value() > defaultMRPConfig->mActiveRetransTimeout)))

        {
            return true;
        }
        return false;
    }

    void LogDetail() const
    {
        if (rotatingIdLen > 0)
        {
            char rotatingIdString[chip::Dnssd::kMaxRotatingIdLen * 2 + 1] = "";
            Encoding::BytesToUppercaseHexString(rotatingId, rotatingIdLen, rotatingIdString, sizeof(rotatingIdString));
            ChipLogDetail(Discovery, "\tRotating ID: %s", rotatingIdString);
        }
        if (strlen(deviceName) != 0)
        {
            ChipLogDetail(Discovery, "\tDevice Name: %s", deviceName);
        }
        if (vendorId > 0)
        {
            ChipLogDetail(Discovery, "\tVendor ID: %u", vendorId);
        }
        if (productId > 0)
        {
            ChipLogDetail(Discovery, "\tProduct ID: %u", productId);
        }
        if (deviceType > 0)
        {
            ChipLogDetail(Discovery, "\tDevice Type: %u", deviceType);
        }
        if (longDiscriminator > 0)
        {
            ChipLogDetail(Discovery, "\tLong Discriminator: %u", longDiscriminator);
        }
        if (strlen(pairingInstruction) != 0)
        {
            ChipLogDetail(Discovery, "\tPairing Instruction: %s", pairingInstruction);
        }
        if (pairingHint > 0)
        {
            ChipLogDetail(Discovery, "\tPairing Hint: %u", pairingHint);
        }
        if (!IsHost(""))
        {
            ChipLogDetail(Discovery, "\tHostname: %s", hostName);
        }
        if (!IsInstanceName(""))
        {
            ChipLogDetail(Discovery, "\tInstance Name: %s", instanceName);
        }
        for (unsigned j = 0; j < numIPs; j++)
        {
#if CHIP_DETAIL_LOGGING
            char buf[Inet::IPAddress::kMaxStringLength];
            char * ipAddressOut = ipAddress[j].ToString(buf);
            ChipLogDetail(Discovery, "\tIP Address #%d: %s", j + 1, ipAddressOut);
            (void) ipAddressOut;
#endif // CHIP_DETAIL_LOGGING
        }
        if (port > 0)
        {
            ChipLogDetail(Discovery, "\tPort: %u", port);
        }
        ChipLogDetail(Discovery, "\tCommissioning Mode: %u", commissioningMode);
        if (mrpRetryIntervalIdle.HasValue())
        {
            ChipLogDetail(Discovery, "\tMrp Interval idle: %" PRIu32 " ms", mrpRetryIntervalIdle.Value().count());
        }
        else
        {
            ChipLogDetail(Discovery, "\tMrp Interval idle: not present");
        }
        if (mrpRetryIntervalActive.HasValue())
        {
            ChipLogDetail(Discovery, "\tMrp Interval active: %" PRIu32 " ms", mrpRetryIntervalActive.Value().count());
        }
        else
        {
            ChipLogDetail(Discovery, "\tMrp Interval active: not present");
        }
    }
};

enum class DiscoveryFilterType : uint8_t
{
    kNone,
    kShortDiscriminator,
    kLongDiscriminator,
    kVendorId,
    kDeviceType,
    kCommissioningMode,
    kInstanceName,
    kCommissioner,
    kCompressedFabricId,
};
struct DiscoveryFilter
{
    DiscoveryFilterType type;
    uint64_t code             = 0;
    const char * instanceName = nullptr;
    DiscoveryFilter() : type(DiscoveryFilterType::kNone), code(0) {}
    DiscoveryFilter(const DiscoveryFilterType newType) : type(newType) {}
    DiscoveryFilter(const DiscoveryFilterType newType, uint64_t newCode) : type(newType), code(newCode) {}
    DiscoveryFilter(const DiscoveryFilterType newType, const char * newInstanceName) : type(newType), instanceName(newInstanceName)
    {}
    bool operator==(const DiscoveryFilter & other) const
    {
        if (type != other.type)
        {
            return false;
        }
        if (type == DiscoveryFilterType::kInstanceName)
        {
            return (instanceName != nullptr) && (other.instanceName != nullptr) && (strcmp(instanceName, other.instanceName) == 0);
        }
        else
        {
            return code == other.code;
        }
    }
};
enum class DiscoveryType
{
    kUnknown,
    kOperational,
    kCommissionableNode,
    kCommissionerNode
};

/// Callbacks for resolving operational node resolution
class OperationalResolveDelegate
{
public:
    virtual ~OperationalResolveDelegate() = default;

    /// Called within the CHIP event loop after a successful node resolution.
    ///
    /// May be called multiple times: implementations may call this once per
    /// received packet and MDNS packets may arrive over different interfaces
    /// which will make nodeData have different content.
    virtual void OnOperationalNodeResolved(const ResolvedNodeData & nodeData) = 0;

    /// Notify a final failure for a node operational resolution.
    ///
    /// Called within the chip event loop if node resolution could not be performed.
    /// This may be due to internal errors or timeouts.
    ///
    /// This will be called only if 'OnOperationalNodeResolved' is never called.
    virtual void OnOperationalNodeResolutionFailed(const PeerId & peerId, CHIP_ERROR error) = 0;
};

/// Callbacks for discovering nodes advertising non-operational status:
///   - Commissioners
///   - Nodes in commissioning modes over IP (e.g. ethernet devices, devices already
///     connected to thread/wifi or devices with a commissioning window open)
class CommissioningResolveDelegate
{
public:
    virtual ~CommissioningResolveDelegate() = default;

    /// Called within the CHIP event loop once a node is discovered.
    ///
    /// May be called multiple times as more nodes send their answer to a
    /// multicast discovery query
    virtual void OnNodeDiscovered(const DiscoveredNodeData & nodeData) = 0;
};

/**
 * Interface for resolving CHIP DNS-SD services
 */
class Resolver
{
public:
    virtual ~Resolver() {}

    /**
     * Initializes the resolver.
     *
     * The method must be called before other methods of this class.
     * If the resolver has already been initialized, the method exits immediately with no error.
     */
    virtual CHIP_ERROR Init(chip::Inet::EndPointManager<Inet::UDPEndPoint> * endPointManager) = 0;

    /**
     * Shuts down the resolver if it has been initialized before.
     */
    virtual void Shutdown() = 0;

    /**
     * If nullptr is passed, the previously registered delegate is unregistered.
     */
    virtual void SetOperationalDelegate(OperationalResolveDelegate * delegate) = 0;

    /**
     * If nullptr is passed, the previously registered delegate is unregistered.
     */
    virtual void SetCommissioningDelegate(CommissioningResolveDelegate * delegate) = 0;

    /**
     * Requests resolution of the given operational node service.
     *
     * This will trigger a DNSSD query.
     *
     * When the operation succeeds or fails, and a resolver delegate has been registered,
     * the result of the operation is passed to the delegate's `OnNodeIdResolved` or
     * `OnNodeIdResolutionFailed` method, respectively.
     */
    virtual CHIP_ERROR ResolveNodeId(const PeerId & peerId, Inet::IPAddressType type) = 0;

    /**
     * Finds all commissionable nodes matching the given filter.
     *
     * Whenever a new matching node is found and a resolver delegate has been registered,
     * the node information is passed to the delegate's `OnNodeDiscoveryComplete` method.
     */
    virtual CHIP_ERROR FindCommissionableNodes(DiscoveryFilter filter = DiscoveryFilter()) = 0;

    /**
     * Finds all commissioner nodes matching the given filter.
     *
     * Whenever a new matching node is found and a resolver delegate has been registered,
     * the node information is passed to the delegate's `OnNodeDiscoveryComplete` method.
     */
    virtual CHIP_ERROR FindCommissioners(DiscoveryFilter filter = DiscoveryFilter()) = 0;

    /**
     * Provides the system-wide implementation of the service resolver
     */
    static Resolver & Instance();
};

} // namespace Dnssd
} // namespace chip
