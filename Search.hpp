#pragma once

#include "Position.hpp"
#include "Move.hpp"

#include <unordered_map>



struct TranspositionTableEntry
{
    enum Flags : uint8_t
    {
        Flag_Invalid,
        Flag_Exact,
        Flag_LowerBound,
        Flag_UpperBound,
    };

    uint64_t positionHash;
    Move move;
    int32_t score = INT32_MIN;
    uint16_t depth = 0;
    Flags flag = Flag_Invalid;
};

class Search
{
public:

    using ScoreType = int32_t;
    static constexpr int32_t CheckmateValue = -1000000;
    static constexpr int32_t InfValue       = 10000000;

    static constexpr int32_t MaxSearchDepth = 64;

    Search();

    ScoreType DoSearch(const Position& position, Move& outBestMove);

private:

    Search(const Search&) = delete;

    struct NegaMaxParam
    {
        const Position* position = nullptr;
        const NegaMaxParam* parentParam = nullptr;
        uint16_t depth;
        uint16_t maxDepth;
        ScoreType alpha;
        ScoreType beta;
        Color color;
    };
    
    struct SearchContext
    {
        uint64_t fh = 0;
        uint64_t fhf = 0;
        uint64_t nodes = 0;
        uint64_t quiescenceNodes = 0;
        uint64_t ttHits = 0;
    };

    struct PvLineEntry
    {
        uint64_t positionHash;
        Move move;
    };

    // principial variation moves tracking for current search
    PackedMove pvArray[MaxSearchDepth][MaxSearchDepth];
    uint16_t pvLengths[MaxSearchDepth];

    // principial variation line from previous iterative deepening search
    uint16_t prevPvArrayLength;
    PvLineEntry prevPvArray[MaxSearchDepth];

    static constexpr uint32_t TranspositionTableSize = 4 * 1024 * 1024;
    std::vector<TranspositionTableEntry> transpositionTable;

    uint64_t searchHistory[2][6][64];

    static constexpr uint32_t NumKillerMoves = 3;
    Move killerMoves[MaxSearchDepth][NumKillerMoves];

    ScoreType QuiescenceNegaMax(const NegaMaxParam& param, SearchContext& ctx);
    ScoreType NegaMax(const NegaMaxParam& param, SearchContext& ctx);

    // check if one of generated moves is in PV table
    void FindPvMove(uint32_t depth, const uint64_t positionHash, MoveList& moves) const;
    void FindHistoryMoves(Color color, MoveList& moves) const;
    void FindKillerMoves(uint32_t depth, MoveList& moves) const;

    static bool IsRepetition(const NegaMaxParam& param);

    // update principal variation line
    void UpdatePvArray(uint32_t depth, const Move move);
};