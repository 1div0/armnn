//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "NeonReshapeWorkload.hpp"

#include "NeonWorkloadUtils.hpp"

#include <arm_compute/runtime/NEON/functions/NEReshapeLayer.h>

#include <boost/polymorphic_cast.hpp>

namespace armnn
{

NeonReshapeWorkload::NeonReshapeWorkload(const ReshapeQueueDescriptor& descriptor,
                                         const WorkloadInfo& info)
    : BaseWorkload<ReshapeQueueDescriptor>(descriptor, info)
{
    m_Data.ValidateInputsOutputs("NeonReshapeWorkload", 1, 1);

    arm_compute::ITensor& input = boost::polymorphic_downcast<IAclTensorHandle*>(m_Data.m_Inputs[0])->GetTensor();
    arm_compute::ITensor& output = boost::polymorphic_downcast<IAclTensorHandle*>(m_Data.m_Outputs[0])->GetTensor();

    auto layer = std::make_unique<arm_compute::NEReshapeLayer>();
    layer->configure(&input, &output);
    m_Layer.reset(layer.release());
}

void NeonReshapeWorkload::Execute() const
{
    ARMNN_SCOPED_PROFILING_EVENT_NEON("NeonReshapeWorkload_Execute");
    m_Layer->run();
}

} //namespace armnn
