//
// re-spirv
//

#include "re-spirv.h"

#include <cassert>
#include <stack>

#define SPV_ENABLE_UTILITY_CODE

#include "spirv/unified1/spirv.h"

// TODO: Replace stderr handling with a consumer printer class.

namespace respv {
    // Common.

    static uint32_t addToList(uint32_t id, IdType idType, uint32_t listIndex, std::vector<ListNode> &listNodes) {
        listNodes.emplace_back(id, idType, listIndex);
        return uint32_t(listNodes.size() - 1);
    }

    static bool SpvHasOperandRange(SpvOp opCode, uint32_t &operandWordStart, uint32_t &operandWordCount) {
        switch (opCode) {
        case SpvOpSelect:
            operandWordStart = 3;
            operandWordCount = 3;
            return true;
        case SpvOpNot:
        case SpvOpBitcast:
            operandWordStart = 3;
            operandWordCount = 1;
            return true;
        case SpvOpIEqual:
        case SpvOpINotEqual:
        case SpvOpUGreaterThan:
        case SpvOpSGreaterThan:
        case SpvOpUGreaterThanEqual:
        case SpvOpSGreaterThanEqual:
        case SpvOpULessThan:
        case SpvOpSLessThan:
        case SpvOpULessThanEqual:
        case SpvOpSLessThanEqual:
        case SpvOpShiftRightLogical:
        case SpvOpShiftRightArithmetic:
        case SpvOpShiftLeftLogical:
        case SpvOpBitwiseOr:
        case SpvOpBitwiseAnd:
        case SpvOpBitwiseXor:
            operandWordStart = 3;
            operandWordCount = 2;
            return true;
        case SpvOpPhi:
            operandWordStart = 3;
            operandWordCount = UINT32_MAX;
            return true;
        case SpvOpBranchConditional:
        case SpvOpSwitch:
            operandWordStart = 1;
            operandWordCount = 1;
            return true;
        default:
            operandWordStart = 0;
            operandWordCount = 0;
            return false;
        }
    }

    // Shader.

    bool Shader::parse(const void *data, size_t size) {
        assert(data != nullptr);
        assert((size % sizeof(uint32_t) == 0) && "Size of data must be aligned to the word size.");

        spirvWords = reinterpret_cast<const uint32_t *>(data);
        spirvWordCount = size / sizeof(uint32_t);
        specConstants.clear();
        specConstantsTargetIds.clear();
        specIdToConstantIndex.clear();
        results.clear();
        decorators.clear();
        blocks.clear();
        listNodes.clear();

        const uint32_t startingWordIndex = 5;
        if (spirvWordCount < startingWordIndex) {
            fprintf(stderr, "Not enough words in SPIR-V.\n");
            return false;
        }

        if (spirvWords[0] != SpvMagicNumber) {
            fprintf(stderr, "Invalid SPIR-V Magic Number on header.\n");
            return false;
        }
        
        if (spirvWords[1] > SpvVersion) {
            fprintf(stderr, "SPIR-V Version is too new for the library. Max version for the library is 0x%X.\n", SpvVersion);
            return false;
        }

        const uint32_t idBound = spirvWords[3];
        instructions.reserve(idBound);
        listNodes.reserve(idBound);
        results.resize(idBound, Result());
        results.shrink_to_fit();

        // Parse all instructions.
        Block block;
        SpvOp opCode = SpvOpNop;
        uint16_t wordCount = 0;
        uint32_t wordIndex = startingWordIndex;
        uint32_t resultId = 0;
        bool hasResult = false;
        bool hasType = false;
        uint32_t operandWordStart = 0;
        uint32_t operandWordCount = 0;
        uint32_t instructionIndex = 0;
        while (wordIndex < spirvWordCount) {
            // Push the new instruction immediately.
            instructionIndex = uint32_t(instructions.size());
            opCode = SpvOp(spirvWords[wordIndex] & 0xFFFFU);
            wordCount = (spirvWords[wordIndex] >> 16U) & 0xFFFFU;

            SpvHasResultAndType(opCode, &hasResult, &hasType);

            if (hasResult) {
                resultId = spirvWords[wordIndex + (hasType ? 2 : 1)];
                if (resultId >= idBound) {
                    fprintf(stderr, "SPIR-V Parsing error. Invalid Result ID: %u.\n", resultId);
                    return false;
                }

                assert(results[resultId].instructionIndex == UINT32_MAX && "Two instructions can't write to the same result.");
                results[resultId].instructionIndex = instructionIndex;
            }
            else {
                resultId = UINT32_MAX;
            }

            switch (opCode) {
            case SpvOpDecorate:
                decorators.emplace_back(instructionIndex);
                break;
            case SpvOpLabel:
                block.startInstructionIndex = instructionIndex;
                break;
            case SpvOpBranch:
            case SpvOpBranchConditional:
            case SpvOpSwitch:
            case SpvOpReturn:
            case SpvOpReturnValue:
            case SpvOpKill:
            case SpvOpUnreachable:
                block.endInstructionIndex = instructionIndex;

                if (block.startInstructionIndex == UINT32_MAX) {
                    fprintf(stderr, "SPIR-V Parsing error. Encountered a termination instruction but no label was defined previously.\n");
                    return false;
                }
                else {
                    blocks.emplace_back(block);
                    block.startInstructionIndex = UINT32_MAX;
                    block.endInstructionIndex = UINT32_MAX;
                }

                break;
            default:
                // Ignore the rest.
                break;
            }

            if (SpvHasOperandRange(opCode, operandWordStart, operandWordCount)) {
                if (wordCount <= operandWordStart) {
                    fprintf(stderr, "SPIR-V Parsing error. Instruction doesn't have enough words for operand count.\n");
                    return false;
                }

                operandWordCount = std::min(uint32_t(wordCount) - operandWordStart, operandWordCount);

                for (uint32_t i = 0; i < operandWordCount; i++) {
                    uint32_t operandId = spirvWords[wordIndex + operandWordStart + i];
                    if (operandId >= idBound) {
                        fprintf(stderr, "SPIR-V Parsing error. Invalid Operand ID: %u.\n", operandId);
                        return false;
                    }

                    bool addResult = (resultId != UINT32_MAX);
                    results[operandId].adjacentListIndex = addToList(addResult ? resultId : instructionIndex, addResult ? IdType::Result : IdType::Instruction, results[operandId].adjacentListIndex, listNodes);
                }
            }

            if (wordCount == 0) {
                fprintf(stderr, "Unknown SPIR-V Parsing error.\n");
                return false;
            }

            instructions.emplace_back(wordIndex, uint32_t(blocks.size() - 1));
            wordIndex += wordCount;
        }

        // Parse all decorations once all instructions have been parsed.
        std::vector<uint32_t> specValues;
        for (const Decorator &decorator : decorators) {
            uint32_t decoratorWordIndex = instructions[decorator.instructionIndex].wordIndex;
            uint32_t decoration = spirvWords[decoratorWordIndex + 2];
            switch (decoration) {
            case SpvDecorationSpecId: {
                uint32_t targetId = spirvWords[decoratorWordIndex + 1];
                const Result &specResult = results[targetId];
                if (specResult.instructionIndex == UINT32_MAX) {
                    fprintf(stderr, "SPIR-V Parsing error. SpvDecorationSpecId targets %u which hasn't been defined before it.\n", targetId);
                    return false;
                }

                uint32_t specWordIndex = instructions[specResult.instructionIndex].wordIndex;
                SpvOp specOpCode = SpvOp(spirvWords[specWordIndex] & 0xFFFFU);
                uint32_t specOpWordCount = (spirvWords[specWordIndex] >> 16U) & 0xFFFFU;
                switch (specOpCode) {
                case SpvOpSpecConstantTrue:
                    specValues.resize(1);
                    specValues[0] = 1;
                    break;
                case SpvOpSpecConstantFalse:
                    specValues.resize(1);
                    specValues[0] = 0;
                    break;
                case SpvOpSpecConstant:
                    specValues.resize(specOpWordCount - 3);
                    memcpy(specValues.data(), &spirvWords[specWordIndex + 3], sizeof(uint32_t) * specValues.size());
                    break;
                case SpvOpSpecConstantComposite:
                    fprintf(stderr, "SPIR-V Parsing error. SpvOpSpecConstantComposite is not supported yet.\n");
                    return false;
                case SpvOpSpecConstantOp:
                    fprintf(stderr, "SPIR-V Parsing error. SpvOpSpecConstantOp is not supported yet.\n");
                    return false;
                default:
                    fprintf(stderr, "SPIR-V Parsing error. SpvDecorationSpecId targets opCode %u which is not valid.\n", specOpCode);
                    return false;
                }

                uint32_t constantId = spirvWords[decoratorWordIndex + 3];
                specConstants.emplace_back(constantId, specValues);
                specConstantsTargetIds.emplace_back(targetId);
                specIdToConstantIndex.resize(std::max(specIdToConstantIndex.size(), size_t(constantId + 1)), UINT32_MAX);
                specIdToConstantIndex[constantId] = uint32_t(specConstants.size() - 1);
                break;
            }
            default:
                break;
            }
        }

        // Indicate the data has been parsed and filled in correctly.
        valid = true;

        return true;
    }

    bool Shader::empty() const {
        return !valid;
    }

    // Optimizer.

    Optimizer::Optimizer() {
        // Empty.
    }

    Optimizer::Optimizer(const void *data, size_t size) {
        parse(data, size);
    }

    bool Optimizer::parse(const void *data, size_t size) {
        return shader.parse(data, size);
    }

    bool Optimizer::empty() const {
        return shader.empty();
    }

    const std::vector<SpecConstant> &Optimizer::getSpecConstants() const {
        return shader.specConstants;
    }

    bool Optimizer::run(const SpecConstant *newSpecConstants, uint32_t newSpecConstantCount, std::vector<uint8_t> &optimizedData) const {
        assert(!empty());

        optimizedData.resize(shader.spirvWordCount * sizeof(uint32_t));
        memcpy(optimizedData.data(), shader.spirvWords, optimizedData.size());
        
        // Patch in specialization constants.
        uint32_t *optimizedWords = reinterpret_cast<uint32_t *>(optimizedData.data());
        for (uint32_t i = 0; i < newSpecConstantCount; i++) {
            const SpecConstant &newSpecConstant = newSpecConstants[i];
            if (newSpecConstant.specId >= shader.specIdToConstantIndex.size()) {
                fprintf(stderr, "Optimization error. Spec Id %u was not detected in the shader.\n", newSpecConstant.specId);
                return false;
            }

            uint32_t specIndex = shader.specIdToConstantIndex[newSpecConstant.specId];
            if (specIndex == UINT32_MAX) {
                fprintf(stderr, "Optimization error. Spec Id %u was not detected in the shader.\n", newSpecConstant.specId);
                return false;
            }

            const SpecConstant &specConstant = shader.specConstants[specIndex];
            if (specConstant.values.size() != newSpecConstant.values.size()) {
                fprintf(stderr, "Optimization error. Spec Id %u expects %zu values but %zu were provided. The value count must be the exact same.\n", newSpecConstant.specId, specConstant.values.size(), newSpecConstant.values.size());
                return false;
            }

            uint32_t targetId = shader.specConstantsTargetIds[specIndex];
            const Result &specResult = shader.results[targetId];
            uint32_t specWordIndex = shader.instructions[specResult.instructionIndex].wordIndex;
            SpvOp specOpCode = SpvOp(shader.spirvWords[specWordIndex] & 0xFFFFU);
            switch (specOpCode) {
            case SpvOpSpecConstantTrue:
            case SpvOpSpecConstantFalse:
                optimizedWords[specWordIndex] = (newSpecConstant.values[0] ? SpvOpConstantTrue : SpvOpConstantFalse) | (optimizedWords[specWordIndex] & 0xFFFF0000U);
                break;
            case SpvOpSpecConstant:
                optimizedWords[specWordIndex] = SpvOpConstant | (optimizedWords[specWordIndex] & 0xFFFF0000U);
                memcpy(&optimizedWords[specWordIndex + 3], newSpecConstant.values.data(), sizeof(uint32_t) * specConstant.values.size());
                break;
            default:
                fprintf(stderr, "Optimization error. Can't patch opCode %u.\n", specOpCode);
                return false;
            }
        }

        return true;
    }

    // Debugger.

    struct Iteration {
        uint32_t id = UINT32_MAX;
        IdType idType = IdType::None;
        uint32_t depth = 0;

        Iteration() {
            // Empty constructor.
        }

        Iteration(uint32_t id, IdType idType, uint32_t depth) {
            this->id = id;
            this->idType = idType;
            this->depth = depth;
        }
    };
    
    void Debugger::printTraversalFrom(const Shader &shader, uint32_t resultId) {
        std::stack<Iteration> iterationStack;
        iterationStack.emplace(resultId, IdType::Result, 0);
        while (!iterationStack.empty()) {
            Iteration it = iterationStack.top();
            iterationStack.pop();

            for (uint32_t i = 0; i < it.depth; i++) {
                fprintf(stdout, "  ");
            }

            if (it.idType == IdType::Result) {
                const Result &result = shader.results[it.id];
                uint32_t resultWordIndex = shader.instructions[result.instructionIndex].wordIndex;
                SpvOp opCode = SpvOp(shader.spirvWords[resultWordIndex] & 0xFFFFU);
                fprintf(stdout, "[%d] %%%d = %s\n", result.instructionIndex, it.id, SpvOpToString(opCode));

                uint32_t listIndex = result.adjacentListIndex;
                while (listIndex != UINT32_MAX) {
                    const ListNode &listNode = shader.listNodes[listIndex];
                    iterationStack.emplace(listNode.id, listNode.idType, it.depth + 1);
                    listIndex = listNode.nextListIndex;
                }
            }
            else if (it.idType == IdType::Instruction) {
                const Instruction &instruction = shader.instructions[it.id];
                SpvOp opCode = SpvOp(shader.spirvWords[instruction.wordIndex] & 0xFFFFU);
                fprintf(stdout, "[%d] %s\n", it.id, SpvOpToString(opCode));
            }
        }
    }
};