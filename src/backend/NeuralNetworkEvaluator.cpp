#include "NeuralNetworkEvaluator.hpp"
#include "Search.hpp"

// enable validation of NN output (check if incremental updates work correctly)
// #define VALIDATE_NETWORK_OUTPUT

// (EXPERIMENTAL) enable accumulator cache
// - doesn't work with multithreading
// - looks like accessing cache lots of cache misses
// #define USE_ACCUMULATOR_CACHE

#ifdef NN_ACCUMULATOR_STATS

static std::atomic<uint64_t> s_NumAccumulatorUpdates = 0;
static std::atomic<uint64_t> s_NumAccumulatorRefreshes = 0;

void NNEvaluator::GetStats(uint64_t& outNumUpdates, uint64_t& outNumRefreshes)
{
    outNumUpdates = s_NumAccumulatorUpdates;
    outNumRefreshes = s_NumAccumulatorRefreshes;
}
void NNEvaluator::ResetStats()
{
    s_NumAccumulatorUpdates = 0;
    s_NumAccumulatorRefreshes = 0;
}

#endif // NN_ACCUMULATOR_STATS

uint32_t PositionToFeaturesVector(const Position& pos, uint16_t* outFeatures, const Color perspective)
{
    uint32_t numFeatures = 0;
    uint32_t numInputs = 0;

    const auto writePieceFeatures = [&](const Bitboard bitboard, const uint32_t bitFlipMask) INLINE_LAMBDA
    {
        bitboard.Iterate([&](uint32_t square) INLINE_LAMBDA
        {
            outFeatures[numFeatures++] = (uint16_t)(numInputs + (square ^ bitFlipMask));
        });
        numInputs += 64;
    };

    const auto& whites = pos.GetSide(perspective);
    const auto& blacks = pos.GetSide(GetOppositeColor(perspective));

    Square whiteKingSquare = whites.GetKingSquare();
    Square blackKingSquare = blacks.GetKingSquare();

    uint32_t bitFlipMask = 0;

    if (whiteKingSquare.File() >= 4)
    {
        // flip file
        whiteKingSquare = whiteKingSquare.FlippedFile();
        blackKingSquare = blackKingSquare.FlippedFile();
        bitFlipMask = 0b000111;
    }

    if (perspective == Color::Black)
    {
        // flip rank
        whiteKingSquare = whiteKingSquare.FlippedRank();
        blackKingSquare = blackKingSquare.FlippedRank();
        bitFlipMask |= 0b111000;
    }

    writePieceFeatures(whites.pawns, bitFlipMask);
    writePieceFeatures(whites.knights, bitFlipMask);
    writePieceFeatures(whites.bishops, bitFlipMask);
    writePieceFeatures(whites.rooks, bitFlipMask);
    writePieceFeatures(whites.queens, bitFlipMask);

    // white king
    {
        const uint32_t whiteKingIndex = 4 * whiteKingSquare.Rank() + whiteKingSquare.File();
        ASSERT(whiteKingIndex < 32);
        outFeatures[numFeatures++] = (uint16_t)(numInputs + whiteKingIndex);
        numInputs += 32;
    }

    writePieceFeatures(blacks.pawns, bitFlipMask);
    writePieceFeatures(blacks.knights, bitFlipMask);
    writePieceFeatures(blacks.bishops, bitFlipMask);
    writePieceFeatures(blacks.rooks, bitFlipMask);
    writePieceFeatures(blacks.queens, bitFlipMask);

    // black king
    {
        outFeatures[numFeatures++] = (uint16_t)(numInputs + blackKingSquare.Index());
        numInputs += 64;
    }

    ASSERT(numInputs == (32 + 64 + 10 * 64));

    return numFeatures;
}

INLINE static uint32_t DirtyPieceToFeatureIndex(const Piece piece, const Color pieceColor, const Square square, const Position& pos, const Color perspective)
{
    // this must match PositionToFeaturesVector !!!

    Square relativeSquare = square;
    {
        // flip the according to the perspective
        if (perspective == Color::Black)
        {
            relativeSquare = relativeSquare.FlippedRank();
        }

        // flip the according to the king placement
        if (pos.GetSide(perspective).GetKingSquare().File() >= 4)
        {
            relativeSquare = relativeSquare.FlippedFile();
        }
    }

    uint32_t index;
    if (piece == Piece::King && pieceColor == perspective)
    {
        // king of the side-to-move is a special case - it can be only present on A-D files
        ASSERT(relativeSquare.File() < 4);
        const uint32_t kingSquareIndex = 4 * relativeSquare.Rank() + relativeSquare.File();
        ASSERT(kingSquareIndex < 32);

        index = 5 * 64; // skip other pieces
        index += kingSquareIndex;
    }
    else
    {
        index = ((uint32_t)piece - (uint32_t)Piece::Pawn) * 64;
        index += relativeSquare.Index();
    }

    // opposite-side pieces features are in second half
    if (pieceColor != perspective) index += 32 + 5 * 64;

    ASSERT(index < (32 + 64 + 10 * 64));

    return index;
}

static uint32_t GetNetworkVariant(const Position& pos)
{
    const uint32_t numPieceCountBuckets = 8;
    const uint32_t pieceCountBucket = std::min(pos.GetNumPiecesExcludingKing() / 4u, numPieceCountBuckets - 1u);
    const uint32_t queenPresenceBucket = pos.Whites().queens || pos.Blacks().queens;
    return queenPresenceBucket * numPieceCountBuckets + pieceCountBucket;
}

int32_t NNEvaluator::Evaluate(const nn::PackedNeuralNetwork& network, const Position& pos)
{
    constexpr uint32_t maxFeatures = 64;

    uint16_t ourFeatures[maxFeatures];
    const uint32_t numOurFeatures = PositionToFeaturesVector(pos, ourFeatures, pos.GetSideToMove());
    ASSERT(numOurFeatures <= maxFeatures);

    uint16_t theirFeatures[maxFeatures];
    const uint32_t numTheirFeatures = PositionToFeaturesVector(pos, theirFeatures, GetOppositeColor(pos.GetSideToMove()));
    ASSERT(numTheirFeatures <= maxFeatures);

    return network.Run(ourFeatures, numOurFeatures, theirFeatures, numTheirFeatures, GetNetworkVariant(pos));
}


#ifdef USE_ACCUMULATOR_CACHE

struct alignas(CACHELINE_SIZE) AccumulatorCacheEntry
{
    bool isValid = false;
    Color perspective = Color::White;
    uint64_t posHash;
    SidePosition posWhite;
    SidePosition posBlack;
    nn::Accumulator accumulator;
};

static constexpr uint32_t c_AccumulatorCacheSize = 8 * 1024;
static AccumulatorCacheEntry c_AccumulatorCache[c_AccumulatorCacheSize];

static bool ReadAccumulatorCache(const Position& pos, const Color perspective, nn::Accumulator& outAccumulator)
{
    const uint64_t posHash = pos.GetHash_NoSideToMove();
    const uint32_t index = posHash % c_AccumulatorCacheSize;
    const AccumulatorCacheEntry& entry = c_AccumulatorCache[index];

    // must have valid entry with matching piece placement and side to move
    if (!entry.isValid ||
        entry.perspective != perspective ||
        entry.posHash != posHash ||
        entry.posWhite != pos.Whites() ||
        entry.posBlack != pos.Blacks())
    {
        return false;
    }

    outAccumulator = entry.accumulator;
    return true;
}

static void WriteAccumulatorCache(const Position& pos, const Color perspective, const nn::Accumulator& accumulator)
{
    const uint64_t posHash = pos.GetHash_NoSideToMove();
    const uint32_t index = posHash % c_AccumulatorCacheSize;
    AccumulatorCacheEntry& entry = c_AccumulatorCache[index];

    // don't overwrite same entry
    if (entry.isValid &&
        entry.perspective == perspective &&
        entry.posWhite == pos.Whites() &&
        entry.posBlack == pos.Blacks())
    {
        return;
    }

    entry.isValid = true;
    entry.perspective = perspective;
    entry.posHash = posHash;
    entry.posWhite = pos.Whites();
    entry.posBlack = pos.Blacks();
    entry.accumulator = accumulator;
}

#endif // USE_ACCUMULATOR_CACHE

static void UpdateAccumulator(const nn::PackedNeuralNetwork& network, const NodeInfo* prevAccumNode, const NodeInfo& node, const Color perspective)
{
    ASSERT(prevAccumNode != &node);
    nn::Accumulator& accumulator = node.nnContext->accumulator[(uint32_t)perspective];
    ASSERT(node.nnContext->accumDirty[(uint32_t)perspective]);

    if (prevAccumNode)
    {
        ASSERT(prevAccumNode->nnContext);
        ASSERT(!prevAccumNode->nnContext->accumDirty[(uint32_t)perspective]);

        constexpr uint32_t maxChangedFeatures = 64;
        uint32_t numAddedFeatures = 0;
        uint32_t numRemovedFeatures = 0;
        uint16_t addedFeatures[maxChangedFeatures];
        uint16_t removedFeatures[maxChangedFeatures];

        // build a list of features to be updated
        for (const NodeInfo* nodePtr = &node; nodePtr != prevAccumNode; nodePtr = nodePtr->parentNode)
        {
            NNEvaluatorContext& nnContext = *(nodePtr->nnContext);

            for (uint32_t i = 0; i < nnContext.numDirtyPieces; ++i)
            {
                const DirtyPiece& dirtyPiece = nnContext.dirtyPieces[i];

                if (dirtyPiece.toSquare.IsValid() && dirtyPiece.fromSquare.IsValid())
                {
                    // TODO use cached accumulator diff for piece move
                }

                if (dirtyPiece.toSquare.IsValid())
                {
                    ASSERT(numAddedFeatures < maxChangedFeatures);
                    const uint16_t featureIdx = (uint16_t)DirtyPieceToFeatureIndex(dirtyPiece.piece, dirtyPiece.color, dirtyPiece.toSquare, node.position, perspective);
                    addedFeatures[numAddedFeatures++] = featureIdx;
                }
                if (dirtyPiece.fromSquare.IsValid())
                {
                    ASSERT(numRemovedFeatures < maxChangedFeatures);
                    const uint16_t featureIdx = (uint16_t)DirtyPieceToFeatureIndex(dirtyPiece.piece, dirtyPiece.color, dirtyPiece.fromSquare, node.position, perspective);
                    removedFeatures[numRemovedFeatures++] = featureIdx;
                }
            }
        }

        // if same feature is present on both lists, it cancels out
        for (uint32_t i = 0; i < numAddedFeatures; ++i)
        {
            for (uint32_t j = 0; j < numRemovedFeatures; ++j)
            {
                if (addedFeatures[i] == removedFeatures[j])
                {
                    addedFeatures[i--] = addedFeatures[--numAddedFeatures];
                    removedFeatures[j--] = removedFeatures[--numRemovedFeatures];
                    break;
                }
            }
        }

#ifdef VALIDATE_NETWORK_OUTPUT
        {
            const uint32_t maxFeatures = 64;
            uint16_t referenceFeatures[maxFeatures];
            const uint32_t numReferenceFeatures = PositionToFeaturesVector(node.position, referenceFeatures, perspective);

            for (uint32_t i = 0; i < numAddedFeatures; ++i)
            {
                bool found = false;
                for (uint32_t j = 0; j < numReferenceFeatures; ++j)
                {
                    if (addedFeatures[i] == referenceFeatures[j]) found = true;
                }
                ASSERT(found);
            }
            for (uint32_t i = 0; i < numRemovedFeatures; ++i)
            {
                for (uint32_t j = 0; j < numReferenceFeatures; ++j)
                {
                    ASSERT(removedFeatures[i] != referenceFeatures[j]);
                }
            }
        }
#endif // VALIDATE_NETWORK_OUTPUT

#ifdef NN_ACCUMULATOR_STATS
        s_NumAccumulatorUpdates++;
#endif // NN_ACCUMULATOR_STATS
        
        if (numAddedFeatures == 0 && numRemovedFeatures == 0)
        {
            accumulator = prevAccumNode->nnContext->accumulator[(uint32_t)perspective];
        }
        else
        {
            accumulator.Update(
                prevAccumNode->nnContext->accumulator[(uint32_t)perspective],
                network.GetAccumulatorWeights(),
                numAddedFeatures, addedFeatures,
                numRemovedFeatures, removedFeatures);
        }
    }
    else // refresh accumulator
    {
        const uint32_t maxFeatures = 64;
        uint16_t features[maxFeatures];
        const uint32_t numFeatures = PositionToFeaturesVector(node.position, features, perspective);
        ASSERT(numFeatures <= maxFeatures);

#ifdef NN_ACCUMULATOR_STATS
        s_NumAccumulatorRefreshes++;
#endif // NN_ACCUMULATOR_STATS

        accumulator.Refresh(
            network.GetAccumulatorWeights(), network.GetAccumulatorBiases(),
            numFeatures, features);
    }

    // mark accumulator as computed
    node.nnContext->accumDirty[(uint32_t)perspective] = false;

#ifdef USE_ACCUMULATOR_CACHE
    // cache accumulator values in PV nodes
    if (node.IsPV())
    {
        WriteAccumulatorCache(node.position, perspective, accumulator);
    }
#endif // USE_ACCUMULATOR_CACHE
}

int32_t NNEvaluator::Evaluate(const nn::PackedNeuralNetwork& network, NodeInfo& node)
{
    ASSERT(node.nnContext);

#ifndef VALIDATE_NETWORK_OUTPUT
    if (node.nnContext->nnScore != InvalidValue)
    {
        return node.nnContext->nnScore;
    }
#endif // VALIDATE_NETWORK_OUTPUT

    const uint32_t refreshCost = node.position.GetNumPieces();

    constexpr Bitboard leftFilesMask = 0x0F0F0F0F0F0F0F0Full;
    const bool kingSides[2] =
    {
        (static_cast<const Position&>(node.position).Whites().king & leftFilesMask) != 0,
        (static_cast<const Position&>(node.position).Blacks().king & leftFilesMask) != 0
    };

    for (const Color perspective : { Color::White, Color::Black })
    {
        // find closest parent node that has valid accumulator
        uint32_t updateCost = 0;
        const NodeInfo* prevAccumNode = nullptr;
        for (const NodeInfo* nodePtr = &node; nodePtr != nullptr; nodePtr = nodePtr->parentNode)
        {
            ASSERT(nodePtr->nnContext);

            updateCost += nodePtr->nnContext->numDirtyPieces;
            if (updateCost > refreshCost)
            {
                // update cost higher than refresh cost, incremental update not worth it
                break;
            }

            // if the king moved across left and right files boundary, then we need to refresh the accumulator
            const bool kingSide = (static_cast<const Position&>(nodePtr->position).GetSide(perspective).king & leftFilesMask) != 0;
            if (kingSide != kingSides[(uint32_t)perspective])
            {
                break;
            }

            if (!nodePtr->nnContext->accumDirty[(uint32_t)perspective])
            {
                // found parent node with valid accumulator
                prevAccumNode = nodePtr;
                break;
            }

#ifdef USE_ACCUMULATOR_CACHE
            // check if accumulator was cached
            if (nodePtr->height < 8 &&
                ReadAccumulatorCache(nodePtr->position,
                    perspective,
                    nodePtr->nnContext->accumulator[(uint32_t)perspective]))
            {
                // found parent node with valid (cached) accumulator
                nodePtr->nnContext->accumDirty[(uint32_t)perspective] = false;
                prevAccumNode = nodePtr;
                break;
            }
#endif // USE_ACCUMULATOR_CACHE
        }

        if (prevAccumNode == &node)
        {
            // do nothing - accumulator is already up to date (was cached)
        }
        else if (node.parentNode && prevAccumNode &&
            node.parentNode != prevAccumNode &&
            node.parentNode->nnContext->accumDirty[(uint32_t)perspective])
        {
            // two-stage update:
            // if parent node has invalid accumulator, update it first
            // this way, sibling nodes can reuse parent's accumulator
            UpdateAccumulator(network, prevAccumNode, *node.parentNode, perspective);
            UpdateAccumulator(network, node.parentNode, node, perspective);
        }
        else
        {
            UpdateAccumulator(network, prevAccumNode, node, perspective);
        }
    }

    const nn::Accumulator& ourAccumulator = node.nnContext->accumulator[(uint32_t)node.position.GetSideToMove()];
    const nn::Accumulator& theirAccumulator = node.nnContext->accumulator[(uint32_t)GetOppositeColor(node.position.GetSideToMove())];

    const int32_t nnOutput = network.Run(ourAccumulator, theirAccumulator, GetNetworkVariant(node.position));

#ifdef VALIDATE_NETWORK_OUTPUT
    {
        const int32_t nnOutputReference = Evaluate(network, node.position);
        ASSERT(nnOutput == nnOutputReference);
    }
    if (node.nnContext->nnScore != InvalidValue)
    {
        ASSERT(node.nnContext->nnScore == nnOutput);
    }
#endif // VALIDATE_NETWORK_OUTPUT

    // cache NN output
    node.nnContext->nnScore = nnOutput;

    return nnOutput;
}
