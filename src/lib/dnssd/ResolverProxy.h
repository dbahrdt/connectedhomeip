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

#include <lib/core/ReferenceCounted.h>
#include <lib/dnssd/Resolver.h>

namespace chip {
namespace Dnssd {

class ResolverDelegateProxy : public ReferenceCounted<ResolverDelegateProxy>,
                              public OperationalResolveDelegate,
                              public CommissioningResolveDelegate

{
public:
    void SetOperationalDelegate(OperationalResolveDelegate * delegate) { mOperationalDelegate = delegate; }
    void SetCommissioningDelegate(CommissioningResolveDelegate * delegate) { mCommissioningDelegate = delegate; }

    // OperationalResolveDelegate
    void OnOperationalNodeResolved(const ResolvedNodeData & nodeData) override
    {
        if (mOperationalDelegate != nullptr)
        {
            mOperationalDelegate->OnOperationalNodeResolved(nodeData);
        }
        else
        {
            ChipLogError(Discovery, "Missing operational delegate. Data discarded.");
        }
    }

    void OnOperationalNodeResolutionFailed(const PeerId & peerId, CHIP_ERROR error) override
    {
        if (mOperationalDelegate != nullptr)
        {
            mOperationalDelegate->OnOperationalNodeResolutionFailed(peerId, error);
        }
        else
        {
            ChipLogError(Discovery, "Missing operational delegate. Failure info discarded.");
        }
    }

    // CommissioningResolveDelegate
    void OnNodeDiscovered(const DiscoveredNodeData & nodeData) override
    {
        if (mCommissioningDelegate != nullptr)
        {
            mCommissioningDelegate->OnNodeDiscovered(nodeData);
        }
        else
        {
            ChipLogError(Discovery, "Missing commissioning delegate. Data discarded.");
        }
    }

private:
    OperationalResolveDelegate * mOperationalDelegate     = nullptr;
    CommissioningResolveDelegate * mCommissioningDelegate = nullptr;
};

class ResolverProxy : public Resolver
{
public:
    ResolverProxy() {}

    // Resolver interface.
    CHIP_ERROR Init(Inet::EndPointManager<Inet::UDPEndPoint> * udpEndPoint = nullptr) override
    {
        ReturnErrorOnFailure(chip::Dnssd::Resolver::Instance().Init(udpEndPoint));
        VerifyOrReturnError(mDelegate == nullptr, CHIP_ERROR_INCORRECT_STATE);
        mDelegate = chip::Platform::New<ResolverDelegateProxy>();
        return mDelegate != nullptr ? CHIP_NO_ERROR : CHIP_ERROR_NO_MEMORY;
    }

    void SetOperationalDelegate(OperationalResolveDelegate * delegate) override
    {
        if (mDelegate != nullptr)
        {
            mDelegate->SetOperationalDelegate(delegate);
        }
        else
        {
            ChipLogError(Discovery, "Failed to proxy operational discovery: missing delegate");
        }
    }

    void SetCommissioningDelegate(CommissioningResolveDelegate * delegate) override
    {
        if (mDelegate != nullptr)
        {
            mDelegate->SetCommissioningDelegate(delegate);
        }
        else
        {
            ChipLogError(Discovery, "Failed to proxy commissioning discovery: missing delegate");
        }
    }

    void Shutdown() override
    {
        VerifyOrReturn(mDelegate != nullptr);
        mDelegate->SetOperationalDelegate(nullptr);
        mDelegate->SetCommissioningDelegate(nullptr);
        mDelegate->Release();
        mDelegate = nullptr;
    }

    CHIP_ERROR ResolveNodeId(const PeerId & peerId, Inet::IPAddressType type) override;
    CHIP_ERROR FindCommissionableNodes(DiscoveryFilter filter = DiscoveryFilter()) override;
    CHIP_ERROR FindCommissioners(DiscoveryFilter filter = DiscoveryFilter()) override;

private:
    ResolverDelegateProxy * mDelegate = nullptr;
};

} // namespace Dnssd
} // namespace chip
