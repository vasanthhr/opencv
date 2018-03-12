// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include "precomp.hpp"
#include "op_inf_engine.hpp"
#include <opencv2/dnn/shape_utils.hpp>

#ifdef HAVE_INF_ENGINE
#include <ie_extension.h>
#include <ie_plugin_dispatcher.hpp>
#endif  // HAVE_INF_ENGINE

namespace cv { namespace dnn {

#ifdef HAVE_INF_ENGINE

InfEngineBackendNode::InfEngineBackendNode(const InferenceEngine::CNNLayerPtr& _layer)
    : BackendNode(DNN_BACKEND_INFERENCE_ENGINE), layer(_layer) {}

void InfEngineBackendNode::connect(std::vector<Ptr<BackendWrapper> >& inputs,
                                   std::vector<Ptr<BackendWrapper> >& outputs)
{
    layer->insData.resize(inputs.size());
    for (int i = 0; i < inputs.size(); ++i)
    {
        InferenceEngine::DataPtr dataPtr = infEngineDataNode(inputs[i]);
        layer->insData[i] = InferenceEngine::DataWeakPtr(dataPtr);
        dataPtr->inputTo[layer->name] = layer;
    }

    CV_Assert(!outputs.empty());

    layer->outData.resize(1);
    InferenceEngine::DataPtr dataPtr = infEngineDataNode(outputs[0]);
    dataPtr->name = layer->name;
    layer->outData[0] = dataPtr;
    dataPtr->creatorLayer = InferenceEngine::CNNLayerWeakPtr(layer);
}

static std::vector<Ptr<InfEngineBackendWrapper> >
infEngineWrappers(const std::vector<Ptr<BackendWrapper> >& ptrs)
{
    std::vector<Ptr<InfEngineBackendWrapper> > wrappers(ptrs.size());
    for (int i = 0; i < ptrs.size(); ++i)
    {
        CV_Assert(!ptrs[i].empty());
        wrappers[i] = ptrs[i].dynamicCast<InfEngineBackendWrapper>();
        CV_Assert(!wrappers[i].empty());
    }
    return wrappers;
}

static InferenceEngine::DataPtr wrapToInfEngineDataNode(const Mat& m, const std::string& name = "")
{
    std::vector<size_t> reversedShape(&m.size[0], &m.size[0] + m.dims);
    std::reverse(reversedShape.begin(), reversedShape.end());
    return InferenceEngine::DataPtr(
      new InferenceEngine::Data(name, reversedShape, InferenceEngine::Precision::FP32)
    );
}

InferenceEngine::TBlob<float>::Ptr wrapToInfEngineBlob(const Mat& m, const std::vector<size_t>& shape,
                                                       InferenceEngine::Layout layout)
{
    return InferenceEngine::make_shared_blob<float>(InferenceEngine::Precision::FP32,
                                                    layout, shape, (float*)m.data);
}

InferenceEngine::TBlob<float>::Ptr wrapToInfEngineBlob(const Mat& m, InferenceEngine::Layout layout)
{
    std::vector<size_t> reversedShape(&m.size[0], &m.size[0] + m.dims);
    std::reverse(reversedShape.begin(), reversedShape.end());
    return wrapToInfEngineBlob(m, reversedShape, layout);
}

InferenceEngine::DataPtr infEngineDataNode(const Ptr<BackendWrapper>& ptr)
{
    CV_Assert(!ptr.empty());
    Ptr<InfEngineBackendWrapper> p = ptr.dynamicCast<InfEngineBackendWrapper>();
    CV_Assert(!p.empty());
    return p->dataPtr;
}

InfEngineBackendWrapper::InfEngineBackendWrapper(int targetId, const cv::Mat& m)
    : BackendWrapper(DNN_BACKEND_INFERENCE_ENGINE, targetId)
{
    dataPtr = wrapToInfEngineDataNode(m);
    blob = wrapToInfEngineBlob(m);
}

InfEngineBackendWrapper::~InfEngineBackendWrapper()
{

}

void InfEngineBackendWrapper::copyToHost()
{

}

void InfEngineBackendWrapper::setHostDirty()
{

}

InfEngineBackendNet::InfEngineBackendNet()
{
    targetDevice = InferenceEngine::TargetDevice::eCPU;
    precision = InferenceEngine::Precision::FP32;
}

InfEngineBackendNet::InfEngineBackendNet(InferenceEngine::CNNNetwork& net)
{
    targetDevice = InferenceEngine::TargetDevice::eCPU;
    precision = InferenceEngine::Precision::FP32;
    inputs = net.getInputsInfo();
    outputs = net.getOutputsInfo();
    layers.resize(net.layerCount());  // A hack to execute InfEngineBackendNet::layerCount correctly.
    initPlugin(net);
}

void InfEngineBackendNet::Release() noexcept
{
    layers.clear();
    inputs.clear();
    outputs.clear();
}

void InfEngineBackendNet::setPrecision(InferenceEngine::Precision p) noexcept
{
    precision = p;
}

InferenceEngine::Precision InfEngineBackendNet::getPrecision() noexcept
{
    return precision;
}

// Assume that outputs of network is unconnected blobs.
void InfEngineBackendNet::getOutputsInfo(InferenceEngine::OutputsDataMap &outputs_) noexcept
{
    outputs_ = outputs;
}
void InfEngineBackendNet::getOutputsInfo(InferenceEngine::OutputsDataMap &outputs_) const noexcept
{
    outputs_ = outputs;
}

// Returns input references that aren't connected to internal outputs.
void InfEngineBackendNet::getInputsInfo(InferenceEngine::InputsDataMap &inputs_) noexcept
{
    inputs_ = inputs;
}

// Returns input references that aren't connected to internal outputs.
void InfEngineBackendNet::getInputsInfo(InferenceEngine::InputsDataMap &inputs_) const noexcept
{
    inputs_ = inputs;
}

InferenceEngine::InputInfo::Ptr InfEngineBackendNet::getInput(const std::string &inputName) noexcept
{
    getInputsInfo(inputs);
    const auto& it = inputs.find(inputName);
    CV_Assert(it != inputs.end());
    return it->second;
}

void InfEngineBackendNet::getName(char*, size_t) noexcept
{
}

size_t InfEngineBackendNet::layerCount() noexcept
{
    return layers.size();
}

InferenceEngine::DataPtr& InfEngineBackendNet::getData(const char *dname) noexcept
{
    CV_Error(Error::StsNotImplemented, "");
    return outputs.begin()->second;  // Just return something.
}

void InfEngineBackendNet::addLayer(const InferenceEngine::CNNLayerPtr &layer) noexcept
{
    layers.push_back(layer);
    inputs.clear();
    outputs.clear();
}

InferenceEngine::StatusCode
InfEngineBackendNet::addOutput(const std::string &layerName, size_t outputIndex,
                               InferenceEngine::ResponseDesc *resp) noexcept
{
    for (const auto& l : layers)
    {
        for (const InferenceEngine::DataPtr& out : l->outData)
        {
            if (out->name == layerName)
            {
                outputs[out->name] = out;
                return InferenceEngine::StatusCode::OK;
            }
        }
    }
    CV_Error(Error::StsObjectNotFound, "Cannot find a layer " + layerName);
    return InferenceEngine::StatusCode::OK;
}

InferenceEngine::StatusCode
InfEngineBackendNet::getLayerByName(const char *layerName, InferenceEngine::CNNLayerPtr &out,
                                    InferenceEngine::ResponseDesc *resp) noexcept
{
    CV_Error(Error::StsNotImplemented, "");
    return InferenceEngine::StatusCode::OK;
}

void InfEngineBackendNet::setTargetDevice(InferenceEngine::TargetDevice device) noexcept
{
    if (device != InferenceEngine::TargetDevice::eCPU &&
        device != InferenceEngine::TargetDevice::eGPU)
        CV_Error(Error::StsNotImplemented, "");
    targetDevice = device;
}

InferenceEngine::TargetDevice InfEngineBackendNet::getTargetDevice() noexcept
{
    return targetDevice;
}

InferenceEngine::StatusCode InfEngineBackendNet::setBatchSize(const size_t size) noexcept
{
    CV_Error(Error::StsNotImplemented, "");
    return InferenceEngine::StatusCode::OK;
}

size_t InfEngineBackendNet::getBatchSize() const noexcept
{
    CV_Error(Error::StsNotImplemented, "");
    return 0;
}

void InfEngineBackendNet::init(int targetId)
{
    if (inputs.empty())
    {
        // Collect all external input blobs.
        inputs.clear();
        std::map<std::string, InferenceEngine::DataPtr> internalOutputs;
        for (const auto& l : layers)
        {
            for (const InferenceEngine::DataWeakPtr& ptr : l->insData)
            {
                InferenceEngine::DataPtr inp(ptr);
                if (internalOutputs.find(inp->name) == internalOutputs.end())
                {
                    InferenceEngine::InputInfo::Ptr inpInfo(new InferenceEngine::InputInfo());
                    inpInfo->setInputData(inp);
                    if (inputs.find(inp->name) == inputs.end())
                        inputs[inp->name] = inpInfo;
                }
            }
            for (const InferenceEngine::DataPtr& out : l->outData)
            {
                // TODO: Replace to uniquness assertion.
                if (internalOutputs.find(out->name) == internalOutputs.end())
                    internalOutputs[out->name] = out;
            }
        }
        CV_Assert(!inputs.empty());
    }

    if (outputs.empty())
    {
        // Add all unconnected blobs to output blobs.
        InferenceEngine::OutputsDataMap unconnectedOuts;
        for (const auto& l : layers)
        {
            // Add all outputs.
            for (const InferenceEngine::DataPtr& out : l->outData)
            {
                // TODO: Replace to uniquness assertion.
                if (unconnectedOuts.find(out->name) == unconnectedOuts.end())
                    unconnectedOuts[out->name] = out;
            }
            // Remove internally connected outputs.
            for (const InferenceEngine::DataWeakPtr& inp : l->insData)
            {
                unconnectedOuts.erase(InferenceEngine::DataPtr(inp)->name);
            }
        }
        CV_Assert(!unconnectedOuts.empty());

        for (auto it = unconnectedOuts.begin(); it != unconnectedOuts.end(); ++it)
        {
            outputs[it->first] = it->second;
        }
    }

    // Set up input blobs.
    inpBlobs.clear();
    for (const auto& it : inputs)
    {
        CV_Assert(allBlobs.find(it.first) != allBlobs.end());
        inpBlobs[it.first] = allBlobs[it.first];
    }

    // Set up output blobs.
    outBlobs.clear();
    for (const auto& it : outputs)
    {
        CV_Assert(allBlobs.find(it.first) != allBlobs.end());
        outBlobs[it.first] = allBlobs[it.first];
    }

    switch (targetId)
    {
    case DNN_TARGET_CPU: setTargetDevice(InferenceEngine::TargetDevice::eCPU); break;
    case DNN_TARGET_OPENCL_FP16: setPrecision(InferenceEngine::Precision::FP16);  // Fallback to the next.
    case DNN_TARGET_OPENCL: setTargetDevice(InferenceEngine::TargetDevice::eGPU); break;
    default:
        CV_Error(Error::StsError, format("Unknown target identifier: %d", targetId));
    }

    if (!isInitialized())
        initPlugin(*this);
}

void InfEngineBackendNet::initPlugin(InferenceEngine::ICNNNetwork& net)
{
    CV_Assert(!isInitialized());

    InferenceEngine::StatusCode status;
    InferenceEngine::ResponseDesc resp;
    const InferenceEngine::Version* v = InferenceEngine::GetInferenceEngineVersion();

    plugin = InferenceEngine::PluginDispatcher({""}).getSuitablePlugin(targetDevice);
    if (std::atoi(v->buildNumber) > 5855)
    {
#ifdef _WIN32
        InferenceEngine::IExtensionPtr extension =
            InferenceEngine::make_so_pointer<InferenceEngine::IExtension>("cpu_extension.dll");
#else
        InferenceEngine::IExtensionPtr extension =
            InferenceEngine::make_so_pointer<InferenceEngine::IExtension>("libcpu_extension.so");
#endif  // _WIN32
        status = plugin->AddExtension(extension, &resp);
        if (status != InferenceEngine::StatusCode::OK)
            CV_Error(Error::StsAssert, resp.msg);
    }
    status = plugin->LoadNetwork(net, &resp);
    if (status != InferenceEngine::StatusCode::OK)
        CV_Error(Error::StsAssert, resp.msg);
}

bool InfEngineBackendNet::isInitialized()
{
    return (bool)plugin;
}

void InfEngineBackendNet::addBlobs(const std::vector<Ptr<BackendWrapper> >& ptrs)
{
    auto wrappers = infEngineWrappers(ptrs);
    for (const auto& wrapper : wrappers)
    {
        allBlobs[wrapper->dataPtr->name] = wrapper->blob;
    }
}

void InfEngineBackendNet::forward()
{
    InferenceEngine::ResponseDesc resp;
    InferenceEngine::StatusCode status = plugin->Infer(inpBlobs, outBlobs, &resp);
    if (status != InferenceEngine::StatusCode::OK)
        CV_Error(Error::StsAssert, resp.msg);
}

Mat infEngineBlobToMat(const InferenceEngine::Blob::Ptr& blob)
{
    // NOTE: Inference Engine sizes are reversed.
    std::vector<size_t> dims = blob->dims();
    std::vector<int> size(dims.begin(), dims.end());
    std::reverse(size.begin(), size.end());
    return Mat(size, CV_32F, (void*)blob->buffer());
}

InfEngineBackendLayer::InfEngineBackendLayer(const InferenceEngine::DataPtr& output_)
{
    output = output_;
}

bool InfEngineBackendLayer::getMemoryShapes(const std::vector<MatShape> &inputs,
                                            const int requiredOutputs,
                                            std::vector<MatShape> &outputs,
                                            std::vector<MatShape> &internals) const
{
    std::vector<size_t> dims = output->dims;
    std::vector<int> shape(dims.begin(), dims.end());
    std::reverse(shape.begin(), shape.end());
    outputs.assign(1, shape);
    return false;
}

bool InfEngineBackendLayer::supportBackend(int backendId)
{
    return backendId == DNN_BACKEND_DEFAULT ||
           backendId == DNN_BACKEND_INFERENCE_ENGINE && haveInfEngine();
}

void InfEngineBackendLayer::forward(std::vector<Mat*> &input, std::vector<Mat> &output,
                                    std::vector<Mat> &internals)
{
    CV_Error(Error::StsError, "Choose Inference Engine as a preferable backend.");
}

void InfEngineBackendLayer::forward(InputArrayOfArrays inputs, OutputArrayOfArrays outputs,
                                    OutputArrayOfArrays internals)
{
    CV_Error(Error::StsInternal, "Choose Inference Engine as a preferable backend.");
}

InferenceEngine::TBlob<int16_t>::Ptr convertFp16(const InferenceEngine::Blob::Ptr& blob)
{
    auto halfs = InferenceEngine::make_shared_blob<int16_t>(InferenceEngine::Precision::FP16, blob->layout(), blob->dims());
    halfs->allocate();
    Mat floatsData(1, blob->size(), CV_32F, blob->buffer());
    Mat halfsData(1, blob->size(), CV_16SC1, halfs->buffer());
    convertFp16(floatsData, halfsData);
    return halfs;
}

#endif  // HAVE_INF_ENGINE

bool haveInfEngine()
{
#ifdef HAVE_INF_ENGINE
    return true;
#else
    return false;
#endif  // HAVE_INF_ENGINE
}

void forwardInfEngine(Ptr<BackendNode>& node)
{
    CV_Assert(haveInfEngine());
#ifdef HAVE_INF_ENGINE
    CV_Assert(!node.empty());
    Ptr<InfEngineBackendNode> ieNode = node.dynamicCast<InfEngineBackendNode>();
    CV_Assert(!ieNode.empty());
    ieNode->net->forward();
#endif  // HAVE_INF_ENGINE
}

}}  // namespace dnn, namespace cv
