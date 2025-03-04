// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "lrn.h"

#include <dnnl_extension_utils.h>
#include <ngraph/opsets/opset1.hpp>
#include <memory_desc/cpu_memory_desc_utils.h>
#include "memory_desc/dnnl_blocked_memory_desc.h"
#include <common/primitive_hashing_utils.hpp>
#include <utils/shape_inference/shape_inference_pass_through.hpp>

#include <memory>
#include <string>

using namespace InferenceEngine;

namespace ov {
namespace intel_cpu {
namespace node {
namespace {

struct LrnKey {
    DnnlMemoryDescCPtr inp0;
    impl_desc_type implType;
    dnnl::algorithm alg;
    size_t size;
    int k;
    float alpha;
    float beta;
    dnnl::primitive_attr attr;

    size_t hash() const;
    bool operator==(const LrnKey& rhs) const;
};

size_t LrnKey::hash() const {
    using namespace dnnl::impl;
    using namespace dnnl::impl::primitive_hashing;

    size_t seed = 0;

    seed = hash_combine(seed, get_md_hash(*inp0->getDnnlDesc().get()));
    seed = hash_combine(seed, implType);
    seed = hash_combine(seed, alg);
    seed = hash_combine(seed, size);
    seed = hash_combine(seed, k);
    seed = hash_combine(seed, alpha);
    seed = hash_combine(seed, beta);

    return seed;
}

bool LrnKey::operator==(const LrnKey &rhs) const {
    bool retVal = true;
    if (inp0 != rhs.inp0) {
        retVal = retVal && inp0 && rhs.inp0 && inp0->getDnnlDesc() == rhs.inp0->getDnnlDesc();
    }

    retVal = retVal && implType == rhs.implType && alg == rhs.alg && size == rhs.size && k == rhs.k &&
             alpha == rhs.alpha && beta == rhs.beta;
    return retVal;
}
} // namespace

bool Lrn::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        auto lrn = ngraph::as_type_ptr<const ngraph::opset1::LRN>(op);
        if (!lrn) {
            errorMessage = "Only opset1 LRN operation is supported";
            return false;
        }

        const auto& dataDims = lrn->get_input_partial_shape(0);
        if (dataDims.size() < 2 || dataDims.size() > 5) {
            errorMessage = "Doesn't support 'data' input with rank: " + std::to_string(dataDims.size());
            return false;
        }
        auto axesNode = ngraph::as_type_ptr<const ngraph::opset1::Constant>(lrn->get_input_node_shared_ptr(1));
        if (!axesNode) {
            errorMessage = "Only Constant operation on 'axis' input is supported";
            return false;
        }

        const auto axes = axesNode->cast_vector<int64_t>();
        const auto dataRank = dataDims.size();
        if (axes.size() == 1 && axes[0] == 1) {
            return true;
        } else {
            std::vector<bool> norm(dataRank, false);
            for (auto &axis : axes) {
                if (axis < 0 || axis >= static_cast<int64_t>(dataRank)) {
                    errorMessage = "Has incorrect reduction axis: " + std::to_string(axis);
                    return false;
                }
                norm[axis] = true;
            }

            for (size_t i = 2; i < norm.size(); ++i) {
                if (!norm[i]) {
                    errorMessage = "Supports only across channels or across spatial reduction";
                    return false;
                }
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

Lrn::Lrn(const std::shared_ptr<ngraph::Node>& op, const GraphContext::CPtr context) :
        Node(op, context, PassThroughShapeInferFactory()) {
    std::string errorMessage;
    if (isSupportedOperation(op, errorMessage)) {
        errorPrefix = "LRN node with name '" + getName() + "'";

        auto lrn = ngraph::as_type_ptr<const ngraph::opset1::LRN>(op);
        auto axes = ngraph::as_type_ptr<const ngraph::opset1::Constant>(lrn->get_input_node_shared_ptr(1))->cast_vector<int64_t>();
        bool isAcrossMaps = (axes.size() == 1 && axes[0] == 1);
        alg = isAcrossMaps ? dnnl::algorithm::lrn_across_channels : dnnl::algorithm::lrn_within_channel;
        alpha = static_cast<float>(lrn->get_alpha());
        beta = static_cast<float>(lrn->get_beta());
        k = static_cast<float>(lrn->get_bias());
        size = lrn->get_nsize();
    } else {
        IE_THROW(NotImplemented) << errorMessage;
    }
}

void Lrn::getSupportedDescriptors() {
    if (!descs.empty())
        return;

    if (getParentEdges().size() != 2)
        IE_THROW() << errorPrefix << " has incorrect number of input edges";
    if (getChildEdges().empty())
        IE_THROW() << errorPrefix << " has incorrect number of output edges";

    InferenceEngine::Precision precision = getOriginalOutputPrecisionAtPort(0);
    if (precision != InferenceEngine::Precision::FP32 && precision != InferenceEngine::Precision::BF16)
        precision = InferenceEngine::Precision::FP32;
    auto inputDataType = DnnlExtensionUtils::IEPrecisionToDataType(precision);

    const auto &parentShape = getInputShapeAtPort(0);

    for (auto format : getAvailableFormatsForDims(parentShape)) {
        auto in_candidate = std::make_shared<DnnlBlockedMemoryDesc>(parentShape, inputDataType, format);
        createDescriptor({in_candidate}, {});
    }
}

std::shared_ptr<MemoryDesc> Lrn::getSrcMemDesc(dnnl::primitive_desc_iterator &primitive_desc_it, size_t idx) {
    if (idx > 0) {
        return std::make_shared<CpuBlockedMemoryDesc>(getOriginalInputPrecisionAtPort(idx), getInputShapeAtPort(idx));
    } else {
        if (getInputShapeAtPort(idx).isDynamic()) {
            return DnnlExtensionUtils::makeUndefinedDesc(primitive_desc_it.src_desc(idx), getInputShapeAtPort(idx));
        }
        return DnnlExtensionUtils::makeDescriptor(primitive_desc_it.src_desc(idx));
    }
}

void Lrn::prepareParams() {
    auto& srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    if (!srcMemPtr || !srcMemPtr->isAllocated())
        IE_THROW() << errorPrefix << " input memory did not allocate";
    if (!dstMemPtr || !dstMemPtr->isAllocated())
        IE_THROW() << errorPrefix << "destination memory did not allocate";

    const NodeDesc* selected_pd = getSelectedPrimitiveDescriptor();
    if (selected_pd == nullptr)
        IE_THROW() << errorPrefix << "preferable primitive descriptor did not set";

    auto inpDesc = getParentEdgeAt(0)->getMemory().GetDescWithType<DnnlMemoryDesc>();

    dnnl::primitive_attr attr;
    attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    LrnKey key = {inpDesc, selected_pd->getImplementationType(), alg, size, k, alpha, beta, attr};
    auto engine = getEngine();

    auto builder = [&engine](const LrnKey& key) -> executorPtr {
        auto prim_desc = dnnl::lrn_forward::primitive_desc(
            engine,
            dnnl::prop_kind::forward_inference,
            key.alg,
            key.inp0->getDnnlDesc(),
            key.inp0->getDnnlDesc(),
            key.size,
            key.alpha,
            key.beta,
            key.k,
            key.attr);

        const bool found = DnnlExtensionUtils::find_implementation(prim_desc, key.implType);

        if (!found)
            return nullptr;

        return std::make_shared<DnnlExecutor>(prim_desc);
    };

    auto cache = context->getParamsCache();
    auto result = cache->getOrCreate(key, builder);
    execPtr = result.first;
    if (!execPtr) {
        IE_THROW() << "Primitive descriptor was not found for node " << getName() << ".";
    }

    auto scratchpadMem = getScratchPadMem(execPtr->getScratchPadDesc());

    primArgs[DNNL_ARG_SCRATCHPAD] = scratchpadMem->GetPrimitive();
    primArgs[DNNL_ARG_SRC] = srcMemPtr->GetPrimitive();
    primArgs[DNNL_ARG_DST] = dstMemPtr->GetPrimitive();
#ifdef CPU_DEBUG_CAPS
    if (result.second == CacheEntryBase::LookUpStatus::Miss) {
        auto pd = execPtr->getPrimitiveDesc();
        DEBUG_LOG("verbose##", getName(), "##", DnnlExtensionUtils::query_pd_info(pd), "\n");
    }
#endif
}

bool Lrn::created() const {
    return getType() == Type::Lrn;
}

void Lrn::createDescriptor(const std::vector<MemoryDescPtr> &inputDesc,
                           const std::vector<MemoryDescPtr> &outputDesc) {
    auto inpDesc = inputDesc[0]->isDefined() ? inputDesc[0] : MemoryDescUtils::makeDummyDesc(*inputDesc[0]);
    DnnlMemoryDescPtr definedInpMemDesc = MemoryDescUtils::convertToDnnlMemoryDesc(inpDesc);
    const auto& in_candidate = definedInpMemDesc->getDnnlDesc();

    auto desc = dnnl::lrn_forward::primitive_desc(
        getEngine(),
        dnnl::prop_kind::forward_inference,
        alg,
        in_candidate,
        in_candidate,
        size,
        alpha,
        beta,
        k);

    descs.push_back(desc);
}

void Lrn::execute(dnnl::stream strm) {
    if (execPtr) {
        execPtr->exec(primArgs, strm);
    } else {
        IE_THROW() << errorPrefix << " doesn't have an initialized executor";
    }
}

void Lrn::executeDynamicImpl(dnnl::stream strm) {
    execute(strm);
}

}   // namespace node
}   // namespace intel_cpu
}   // namespace ov
