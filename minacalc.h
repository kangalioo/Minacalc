#pragma once
#include "NoteDataStructures.h"
#include <vector>

// For internal, must be preprocessor defined
#if defined(MINADLL_COMPILE) && defined(_WIN32)
#define MINACALC_API __declspec(dllexport)
#endif

// For Stepmania
#ifndef MINACALC_API
#define MINACALC_API
#endif

typedef std::vector<DifficultyRating> MinaSD;
typedef std::vector<std::vector<float>> Finger;
typedef std::vector<Finger> ProcessedFingers;
typedef std::vector<float> JackSeq; // Vector of a local jack speed difficulty for each row

enum ChiselType { STREAM, JS, HS, TECH, JACK };

// The comments in here contain the concept of 'points'. That's
// referring to Wifescore points, but scaled to a max of 1 (instead of
// 2 as usually)

/* The difficulties of each hand tend to be independent from one another. This
is not absolute, as in the case of polyrhythm trilling. However the goal of the
calculator is to estimate the difficulty of a file given the physical properties
of such, and not to evalute the difficulty of reading (which is much less
quantifiable). It is both less accurate and logically incorrect to attempt to
assert a single difficulty for both hands for a given interval of time in a
file, so most of the internal calculator operations are done after splitting up
each track of the chart into their respective phalangeal parents. */  // This is stupid but whatever
class Hand
{
public:
    /* Spits out a rough estimate of difficulty based on the ms values within
    the interval The vector passed to it is the vector of ms values within each
    interval, and not the full vector of intervals. */
    static float CalcMSEstimate(std::vector<float>& input);

    /* Averages nps and ms estimates for difficulty to get a rough initial
    value. This is relatively robust as patterns that get overrated by nps
    estimates are underrated by ms estimates, and vice versa. Pattern modifiers
    are used to adjust for circumstances in which this is not true. The result
    is output to v_itvNPSdiff and v_itvMSdiff. */
    void InitDiff(Finger& f1, Finger& f2);

    // Totals up the points available for each interval
    void InitPoints(const Finger& f1, const Finger& f2);

    /* The stamina model works by asserting a minimum difficulty relative to
    the supplied player skill level for which the player's stamina begins to
    wane. Experience in both gameplay and algorithm testing has shown the
    appropriate value to be around 0.8. The multiplier is scaled to the
    proportionate difference in player skill. */
    void StamAdjust(float x, std::vector<float>& diff);
    
    /* For a given player skill level x, invokes the function used by wife
    scoring to assert the average of the distribution of point gain for each
    interval and then tallies up the result to produce an average total number
    of points achieved by this hand. */
    float CalcInternal(float x, ChiselType flags, bool stam);

    float fingerbias;
    std::vector<float> ohjumpscale, rollscale, hsscale, jumpscale, anchorscale;
    std::vector<int> v_itvpoints; // Max points for each interval
    std::vector<float> v_itvNPSdiff, v_itvMSdiff; // Calculated difficulty for each interval
private:
    // Do we moving average the difficulty intervals?
    const bool SmoothDifficulty = true;
    
    float basescaler = 2.564f * 1.05f * 1.1f * 1.10f * 1.10f *
                        1.025; // multiplier to standardize baselines

    // Stamina Model params
    const float ceil = 1.08f; // stamina multiplier max
    const float mag = 355.f; // multiplier generation scaler
    const float fscale = 2000.f; // how fast the floor rises (it's lava)
    const float prop = 0.75f; // proportion of player difficulty at which stamina tax begins
};

class Calc
{
public:
    /* Primary calculator function that wraps everything else. Initializes the
    hand objects and then runs the chisel function under varying circumstances
    to estimate difficulty for each different skillset. Currently only
    overall/stamina are being produced. */
    DifficultyRating CalcMain(const std::vector<NoteInfo>& NoteInfo, float music_rate, float score_goal);

    // redo these asap
    // Calculates the amount of points a player with player skill
    // `x` will lose on a JackSeq `j`
    static float JackLoss(const std::vector<float>& j, float x);
    // t=track index
    // Generates a JackSeq from NoteInfo
    static JackSeq SequenceJack(const std::vector<NoteInfo>& NoteInfo, unsigned int t, float music_rate);
    
    // Number of intervals
    int numitv;
    
    float CalculateFingerbias(const std::vector<NoteInfo>& NoteInfo, unsigned int finger1, unsigned int finger2);
    
    void Init(const std::vector<NoteInfo>& note_info, float music_rate, float score_goal);

    /* Splits up the chart by each hand and calls ProcessFinger on each "track"
    before passing
    the results to the hand initialization functions. Also passes the input
    timingscale value. */
    void InitHand(Hand& hand, const std::vector<NoteInfo>& note_info, int f1, int f2, float music_rate);

    /* Slices the track into predefined intervals of time. All taps within each
    interval have their ms values from the last note in the same column
    calculated and the result is spit out
    into a new Finger object, or vector of vectors of floats (ms from last note
    in the track). */
    Finger ProcessFinger(const std::vector<NoteInfo>& NoteInfo, unsigned int t, float music_rate);

    float MaxPoints = 0.f; // Total points achievable in the file

    /* Returns estimate of player skill needed to achieve score goal on chart.
     * The player_skill parameter gives an initial guess and floor for player skill.
     * Resolution relates to how precise the answer is.
     * Additional parameters give specific skill sets being tested for.*/
    float Chisel(float player_skill,
                 float resolution,
                 float score_goal,
                 ChiselType type,
                 bool stam);
    
    // Used in Chisel()
    float CalcScoreForPlayerSkill(float player_skill, ChiselType type, bool stam);

    std::vector<float> OHJumpDownscaler(const std::vector<NoteInfo>& NoteInfo,
                                        unsigned int t1,
                                        unsigned int t2);
    std::vector<float> Anchorscaler(const std::vector<NoteInfo>& NoteInfo,
                                    unsigned int t1,
                                    unsigned int t2);
    std::vector<float> HSDownscaler(const std::vector<NoteInfo>& NoteInfo);
    std::vector<float> JumpDownscaler(const std::vector<NoteInfo>& NoteInfo);
    std::vector<float> RollDownscaler(const Finger& f1, const Finger& f2);

    Hand left_hand;
    Hand right_hand;

private:
    float fingerbias;
    std::vector<std::vector<int>> nervIntervals;

    // Const calc params
    const bool SmoothPatterns = true; // Do we moving average the pattern modifier intervals?
    const float IntervalSpan = 0.5f; // Intervals of time we slice the chart at
    const bool logpatterns = false;

    JackSeq j0;
    JackSeq j1;
    JackSeq j2;
    JackSeq j3;
};

MINACALC_API DifficultyRating
MinaSDCalc(const std::vector<NoteInfo>& NoteInfo,
           float musicrate,
           float goal);
MINACALC_API MinaSD
MinaSDCalc(const std::vector<NoteInfo>& NoteInfo);
MINACALC_API int
GetCalcVersion();

/*
To get debug output:
1. Turn off the built in showlog window
2. Do StepMania.exe >> out.txt in cmd
*/
