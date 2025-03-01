//*****************************************************************************
// Copyright 2024 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include <atomic>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4005 4309 6001 6385 6386 6326 6011 6246 4005)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/port/canonical_errors.h"
#pragma GCC diagnostic pop
#pragma warning(pop)

#include <openvino/genai/continuous_batching_pipeline.hpp>

#include "../http_payload.hpp"
#include "../profiler.hpp"
#include "apis/openai_completions.hpp"
#include "llmnoderesources.hpp"
#include "text_processor.hpp"

using namespace ovms;

namespace mediapipe {

#define IGNORE_EOS_MAX_TOKENS_LIMIT 4000

using InputDataType = ovms::HttpPayload;
using OutputDataType = std::string;

const std::string LLM_SESSION_SIDE_PACKET_TAG = "LLM_NODE_RESOURCES";

static std::string packIntoServerSideEventMessage(const std::string& message) {
    std::stringstream ss;
    ss << "data: " << message << "\n\n";
    return ss.str();
}

// CB lib internals rely on request_id, so for now we provide increasing ID
static std::atomic<uint64_t> currentRequestId = 0;

class HttpLLMCalculator : public CalculatorBase {
    std::shared_ptr<ovms::LLMNodeResources> nodeResources;
    ov::genai::GenerationHandle generationHandle;
    std::shared_ptr<OpenAIChatCompletionsHandler> apiHandler;
    std::shared_ptr<ClientConnection> client;

    std::shared_ptr<ov::genai::TextCallbackStreamer> streamer;
    std::string lastStreamerCallbackOutput;

    static const std::string INPUT_TAG_NAME;
    static const std::string OUTPUT_TAG_NAME;
    static const std::string LOOPBACK_TAG_NAME;

    mediapipe::Timestamp timestamp{0};

public:
    static absl::Status GetContract(CalculatorContract* cc) {
        RET_CHECK(!cc->Inputs().GetTags().empty());
        RET_CHECK(!cc->Outputs().GetTags().empty());
        cc->Inputs().Tag(INPUT_TAG_NAME).Set<InputDataType>();
        cc->Inputs().Tag(LOOPBACK_TAG_NAME).Set<bool>();
        cc->InputSidePackets().Tag(LLM_SESSION_SIDE_PACKET_TAG).Set<ovms::LLMNodeResourcesMap>();
        cc->Outputs().Tag(OUTPUT_TAG_NAME).Set<OutputDataType>();
        cc->Outputs().Tag(LOOPBACK_TAG_NAME).Set<bool>();
        return absl::OkStatus();
    }

    absl::Status Close(CalculatorContext* cc) final {
        OVMS_PROFILE_FUNCTION();
        SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "LLMCalculator [Node: {} ] Close", cc->NodeName());
        return absl::OkStatus();
    }

    absl::Status Open(CalculatorContext* cc) final {
        OVMS_PROFILE_FUNCTION();
        SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "LLMCalculator  [Node: {}] Open start", cc->NodeName());
        ovms::LLMNodeResourcesMap nodeResourcesMap = cc->InputSidePackets().Tag(LLM_SESSION_SIDE_PACKET_TAG).Get<ovms::LLMNodeResourcesMap>();
        auto it = nodeResourcesMap.find(cc->NodeName());
        RET_CHECK(it != nodeResourcesMap.end()) << "Could not find initialized LLM node named: " << cc->NodeName();
        nodeResources = it->second;
        SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "LLMCalculator [Node: {}] Open end", cc->NodeName());
        return absl::OkStatus();
    }

    absl::Status Process(CalculatorContext* cc) final {
        OVMS_PROFILE_FUNCTION();
        RET_CHECK(this->nodeResources != nullptr);

        // For cases where MediaPipe decides to trigger Process() when there are no inputs
        if (cc->Inputs().Tag(INPUT_TAG_NAME).IsEmpty() && cc->Inputs().Tag(LOOPBACK_TAG_NAME).IsEmpty()) {
            return absl::OkStatus();
        }
        try {
            // First iteration of Process()
            if (!cc->Inputs().Tag(INPUT_TAG_NAME).IsEmpty()) {
                OVMS_PROFILE_SCOPE("Deserialization of first request");
                // Check if we did not receive the payload twice
                RET_CHECK(this->apiHandler == nullptr);
                RET_CHECK(this->generationHandle == nullptr);
                RET_CHECK(this->streamer == nullptr);
                RET_CHECK(this->client == nullptr);

                InputDataType payload = cc->Inputs().Tag(INPUT_TAG_NAME).Get<InputDataType>();
                SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "Request body: {}", payload.body);
                SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "Request uri: {}", payload.uri);
                Endpoint endpoint;
                if (payload.uri == "/v3/chat/completions" || payload.uri == "/v3/v1/chat/completions") {
                    endpoint = Endpoint::CHAT_COMPLETIONS;
                } else if (payload.uri == "/v3/completions" || payload.uri == "/v3/v1/completions") {
                    endpoint = Endpoint::COMPLETIONS;
                } else {
                    return absl::InvalidArgumentError("Wrong endpoint. Allowed endpoints: /v3/chat/completions, /v3/completions");
                }
                this->apiHandler = std::make_shared<OpenAIChatCompletionsHandler>(*payload.parsedJson, endpoint, std::chrono::system_clock::now(),
                    nodeResources->cbPipe->get_tokenizer());
                this->client = payload.client;

                auto status = this->apiHandler->parseRequest(nodeResources->maxTokensLimit, nodeResources->bestOfLimit, nodeResources->isSpeculativePipeline);
                if (status != absl::OkStatus())
                    return status;

                std::string finalPrompt = "";
                bool encodeAddSpecialTokens = false;
                switch (endpoint) {
                case Endpoint::CHAT_COMPLETIONS: {
                    bool success;
                    if (this->apiHandler->getProcessedJson().size() > 0) {
                        success = TextProcessor::applyChatTemplate(this->nodeResources->textProcessor, this->nodeResources->modelsPath, this->apiHandler->getProcessedJson(), finalPrompt);
                    } else {
                        success = TextProcessor::applyChatTemplate(this->nodeResources->textProcessor, this->nodeResources->modelsPath, payload.body, finalPrompt);
                    }
                    if (!success) {
                        return absl::Status(absl::StatusCode::kInvalidArgument, finalPrompt);
                    }
                    if (finalPrompt.size() == 0) {
                        return absl::Status(absl::StatusCode::kInvalidArgument, "Final prompt after applying chat template is empty");
                    }
                    break;
                }
                case Endpoint::COMPLETIONS: {
                    finalPrompt = this->apiHandler->getPrompt().value();
                    encodeAddSpecialTokens = true;
                }
                }

                {
                    OVMS_PROFILE_SCOPE("pipeline->add_request()");

                    // Check if client disconnected while waiting in HTTP requests queue
                    if (this->client->isDisconnected()) {
                        return absl::CancelledError();
                    }
                    SPDLOG_LOGGER_TRACE(llm_calculator_logger, "Final prompt: {}", finalPrompt);
                    SPDLOG_LOGGER_TRACE(llm_calculator_logger, "encodeAddSpecialTokens: {}", encodeAddSpecialTokens);
                    ov::Tensor finalPromptIds = nodeResources->cbPipe->get_tokenizer().encode(finalPrompt, ov::genai::add_special_tokens(encodeAddSpecialTokens)).input_ids;
                    this->apiHandler->setPromptTokensUsage(finalPromptIds.get_size());
                    SPDLOG_LOGGER_TRACE(llm_calculator_logger, "{}", getPromptTokensString(finalPromptIds));

                    this->generationHandle = nodeResources->cbPipe->add_request(
                        currentRequestId++, /*to be removed from API?*/
                        finalPromptIds,
                        this->apiHandler->createGenerationConfig());

                    this->client->registerDisconnectionCallback([genHandle = this->generationHandle]() {
                        genHandle->drop();
                    });
                }
                nodeResources->notifyExecutorThread();

                auto callback = [this](std::string text) {
                    SPDLOG_LOGGER_TRACE(llm_calculator_logger, "Streamer callback executed with text: [{}]", text);
                    this->lastStreamerCallbackOutput = text;
                    return false;
                };

                this->streamer = std::make_shared<ov::genai::TextCallbackStreamer>(nodeResources->cbPipe->get_tokenizer(), callback);
            }

            RET_CHECK(this->generationHandle != nullptr);
            RET_CHECK(this->apiHandler != nullptr);
            RET_CHECK(this->streamer != nullptr);
            RET_CHECK(this->client != nullptr);

            if (this->client->isDisconnected()) {
                return absl::CancelledError();
            }

            // Unary scenario
            if (!this->apiHandler->isStream()) {
                OVMS_PROFILE_SCOPE("Unary generation cycle");

                std::vector<ov::genai::GenerationOutput> generationOutputs = this->generationHandle->read_all();
                if (this->generationHandle->get_status() == ov::genai::GenerationStatus::DROPPED_BY_HANDLE) {
                    return absl::CancelledError();
                }
                RET_CHECK(generationOutputs.size() >= 1);
                std::string response = this->apiHandler->serializeUnaryResponse(generationOutputs);
                SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "Complete unary response: {}", response);
                cc->Outputs().Tag(OUTPUT_TAG_NAME).Add(new OutputDataType{std::move(response)}, timestamp);
            } else {
                OVMS_PROFILE_SCOPE("Stream generation cycle");
                // Streaming scenario
                // Each iteration is single execution of Process() method

                if (this->generationHandle->get_status() == ov::genai::GenerationStatus::DROPPED_BY_HANDLE) {
                    return absl::CancelledError();
                }

                if (this->generationHandle->get_status() == ov::genai::GenerationStatus::RUNNING || this->generationHandle->can_read()) {
                    // Subsequent iteration
                    OVMS_PROFILE_SCOPE("Generation of subsequent streaming response");
                    ov::genai::GenerationOutputs generationOutputs = this->generationHandle->read();
                    RET_CHECK(generationOutputs.size() == 1);  // TODO: Support multiple generations
                    this->apiHandler->incrementProcessedTokens(generationOutputs.begin()->second.generated_ids.size());
                    auto generationOutput = generationOutputs.begin()->second;
                    // This loop could be handled in the streamer, but for now we want to keep it identical with GenAI
                    // so such change should be done in GenAI first
                    std::stringstream ss;
                    for (const auto& token : generationOutput.generated_ids) {
                        this->streamer->put(token);
                        ss << this->lastStreamerCallbackOutput;
                    }
                    std::string lastTextChunk = ss.str();
                    ov::genai::GenerationFinishReason finishReason = generationOutputs.begin()->second.finish_reason;
                    if (finishReason == ov::genai::GenerationFinishReason::NONE) {  // continue
                        if (lastTextChunk.size() > 0) {
                            std::string response = packIntoServerSideEventMessage(this->apiHandler->serializeStreamingChunk(lastTextChunk, finishReason));
                            SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "Generated subsequent streaming response: {}", response);
                            cc->Outputs().Tag(OUTPUT_TAG_NAME).Add(new OutputDataType{std::move(response)}, timestamp);
                        }
                        cc->Outputs().Tag(LOOPBACK_TAG_NAME).Add(new bool{true}, timestamp);
                    } else {  // finish generation
                        OVMS_PROFILE_SCOPE("Generation of last streaming response");
                        this->streamer->end();
                        // if streamer::put returned a value, streamer::end() result will not contain it, so we add it manually
                        if (!this->lastStreamerCallbackOutput.empty())
                            lastTextChunk = lastTextChunk + this->lastStreamerCallbackOutput;
                        std::string response = packIntoServerSideEventMessage(this->apiHandler->serializeStreamingChunk(lastTextChunk, finishReason));
                        if (this->apiHandler->getStreamOptions().includeUsage)
                            response += packIntoServerSideEventMessage(this->apiHandler->serializeStreamingUsageChunk());

                        response += packIntoServerSideEventMessage("[DONE]");

                        SPDLOG_LOGGER_DEBUG(llm_calculator_logger, "Generated complete streaming response: {}", response);
                        cc->Outputs().Tag(OUTPUT_TAG_NAME).Add(new OutputDataType{std::move(response)}, timestamp);
                    }
                }
            }
        } catch (ov::AssertFailure& e) {
            return absl::InvalidArgumentError(e.what());
        } catch (...) {
            return absl::InvalidArgumentError("Response generation failed");
        }
        timestamp = timestamp.NextAllowedInStream();

        return absl::OkStatus();
    }
};

const std::string HttpLLMCalculator::INPUT_TAG_NAME{"HTTP_REQUEST_PAYLOAD"};
const std::string HttpLLMCalculator::OUTPUT_TAG_NAME{"HTTP_RESPONSE_PAYLOAD"};
const std::string HttpLLMCalculator::LOOPBACK_TAG_NAME{"LOOPBACK"};

REGISTER_CALCULATOR(HttpLLMCalculator);

}  // namespace mediapipe
