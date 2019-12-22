#include "minacalc.h"
#include <cmath>
#include <iostream>
#include <algorithm>
#include <memory>
#include <numeric>
#include <iostream>

using std::cout;
using std::endl;

using std::vector;
using std::min;
using std::max;
using std::sqrt;
using std::pow;

template<typename T>
T CalcClamp(T x, T l, T h) {
return x > h ? h : (x < l ? l : x);
}

template <typename F>
float approximate(float value, float resolution, int num_iters, F is_too_low, bool limit_at_100 = false) {
    for (int i = 0; i < num_iters; i++) {
        while (is_too_low(value)) {
            if (limit_at_100 && value > 100.f) return value;
            value += resolution;
        }
        value -= resolution;
        resolution /= 2.f;
    }
    
    return value + 2.f * resolution;
}

inline float mean(const vector<float>& v) {
    return std::accumulate(begin(v), end(v), 0.f) / v.size();
}

// Coefficient of variation
inline float cv(const vector<float> &input) {
    float sd = 0.f;
    float average = mean(input);
    for (float i : input)
        sd += (i - average)*(i - average);

    return sqrt(sd / input.size()) / average;
}

inline float downscale_low_accuracy_scores(float f, float sg) {
    if (sg >= 0.93f) return f;
    // This clamp is practically useless I think
    return CalcClamp(f - sqrt(0.93f - sg), 0.f, 100.f);
}

// Moving average with n=3. The `neutral` value is used for the
// "out-of-bounds values" required for the moving averages on the start
// and end.
inline void Smooth(vector<float>& input, float neutral) {
    float f1;
    float f2 = neutral;
    float f3 = neutral;

    for (float & i : input) {
        f1 = f2;
        f2 = f3;
        f3 = i;
        i = (f1 + f2 + f3) / 3;
    }
}

// Like `Smooth()`, but with n=2 and neutral value zero.
inline void DifficultyMSSmooth(vector<float>& input) {
    float f1;
    float f2 = 0.f;

    for (float & i : input) {
        f1 = f2;
        f2 = i;
        i = (f1 + f2) / 2.f;
    }
}

// Returns approximately the skillset rating plus 0.609 (That number
// varies a little depending on the variations of the skillsets)
inline float AggregateScores(const vector<float>& skillsets, float rating, float resolution) {
    auto check_if_too_low = [skillsets](float rating) {
        float sum = 0.0f;
        for (float i : skillsets) {
            sum += 2.f / std::erfc(0.5f * (i - rating)) - 1.f;
        }
        return 3 < sum;
    };
    return approximate(rating, resolution, 11, check_if_too_low);
}

// Converts a row byte into the number of taps present in the row
unsigned int column_count(unsigned int note) {
    return note % 2 + note / 2 % 2 + note / 4 % 2 + note / 8 % 2;
}

// Proportion of how many taps belong to chords of size `chord_size`
float chord_proportion(const vector<NoteInfo>& NoteInfo, const int chord_size) {
    unsigned int taps = 0;
    unsigned int chords = 0;

    for (auto row : NoteInfo) {
        unsigned int notes = column_count(row.notes);
        taps += notes;
        if (notes == chord_size)
            chords += notes;
    }

    return static_cast<float>(chords) / static_cast<float>(taps);
}

vector<float> skillset_vector(const DifficultyRating& difficulty) {
    return vector<float> {difficulty.overall,
                          difficulty.stream,
                          difficulty.jumpstream,
                          difficulty.handstream,
                          difficulty.stamina,
                          difficulty.jack,
                          difficulty.chordjack,
                          difficulty.technical
    };
}

float highest_difficulty(const DifficultyRating& difficulty) {
    auto v = {difficulty.stream,difficulty.jumpstream,difficulty.handstream,difficulty.stamina,difficulty.jack,
              difficulty.chordjack,difficulty.technical};
    return *std::max_element(v.begin(), v.end());
}

void Calc::Init(const vector<NoteInfo>& note_info, float music_rate, float score_goal) {
    // Number of intervals
    numitv = static_cast<int>(std::ceil(note_info.back().rowTime / (music_rate * IntervalSpan)));

    nervIntervals = vector<vector<int>>(numitv, vector<int>());
    InitHand(left_hand, note_info, 0, 1, music_rate);
    InitHand(right_hand, note_info, 2, 3, music_rate);

    j0 = SequenceJack(note_info, 0, music_rate);
    j1 = SequenceJack(note_info, 1, music_rate);
    j2 = SequenceJack(note_info, 2, music_rate);
    j3 = SequenceJack(note_info, 3, music_rate);
    
    // Calculate total max points
    MaxPoints = 0;
    for (size_t i = 0; i < left_hand.v_itvpoints.size(); i++)
        MaxPoints += static_cast<float>(left_hand.v_itvpoints[i] + right_hand.v_itvpoints[i]);
    
    // The base fingerbias value is calculated in Anchorscaler() which
    // is called by InitializeHands(). That should definitely be moved
    // to here in the future, because we don't want to have one single
    // calculation in a thousand pieces all over the place!
    fingerbias /= static_cast<float>(2 * nervIntervals.size());
}

void Calc::InitHand(Hand& hand, const vector<NoteInfo>& note_info, int f1, int f2, float music_rate) {
    Finger finger1 = ProcessFinger(note_info, f1, music_rate);
    Finger finger2 = ProcessFinger(note_info, f2, music_rate);
    
    hand.InitDiff(finger1, finger2);
    hand.InitPoints(finger1, finger2);
    
    hand.ohjumpscale = OHJumpDownscaler(note_info, 1 << f1, 1 << f2);
    hand.anchorscale = Anchorscaler(note_info, 1 << f1, 1 << f2);
    hand.rollscale = RollDownscaler(finger1, finger2);
    hand.hsscale = HSDownscaler(note_info);
    hand.jumpscale = JumpDownscaler(note_info);
}

DifficultyRating Calc::CalcMain(const vector<NoteInfo>& NoteInfo, float music_rate, float score_goal) {
    Init(NoteInfo, music_rate, score_goal);
    
    float last_row_time = NoteInfo.back().rowTime;
    
    // last_row_time: 30 -> 0.93; 60 -> 1.00
    float grindscaler = 0.93f + 0.07f * CalcClamp(last_row_time / 30.f - 1.f, 0.f, 1.f);
    // last_row_time: 9.8 -> 0.87; 234.8 -> 1.00
    grindscaler *= CalcClamp(0.873f + (0.13f * (last_row_time / 15.f - 1.f)), 0.87f, 1.f);
    
    // last_row_time: 150 -> 0.9; 300 -> 1.0
    float shortstamdownscaler = CalcClamp(0.9f + (0.1f * (last_row_time - 150.f) / 150.f), 0.9f, 1.f);

    float jprop = chord_proportion(NoteInfo, 2);
    // jprop: -0.5 -> 0.8; 0.5 -> 1
    // jprop: 0 -> 0.9; 0.5 -> 1
    float nojumpsdownscaler = CalcClamp(0.8f + (0.2f * (jprop + 0.5f)), 0.8f, 1.f);
    // jprop: 0.43 -> 1; 0.85 -> 0.85
    float manyjumpsdownscaler = CalcClamp(1.43f - jprop, 0.85f, 1.f);

    float hprop = chord_proportion(NoteInfo, 3);
    // hprop: -0.75 -> 0.8; 0.25 -> 1
    // hprop: 0 -> 0.95; 0.25 -> 1
    float nohandsdownscaler = CalcClamp(0.8f + (0.2f * (hprop + 0.75f)), 0.8f, 1.f);
    // hprop: 0.23 -> 1; 0.38 -> 0.85
    float allhandsdownscaler = CalcClamp(1.23f - hprop, 0.85f, 1.f);

    float qprop = chord_proportion(NoteInfo, 4);
    // qprop: 0.13 -> 1; 0.28 -> 0.85
    float lotquaddownscaler = CalcClamp(1.13f - qprop, 0.85f, 1.f);

    // jprop + hprop: 0.625 -> 1; 0.775 -> 0.85
    float jumpthrill = CalcClamp(1.625f - jprop - hprop, 0.85f, 1.f);

    float stream = Chisel(0.1f, 10.24f, score_goal, CHISEL_NPS);
    float js = Chisel(0.1f, 10.24f, score_goal, CHISEL_NPS | CHISEL_JS);
    float hs = Chisel(0.1f, 10.24f, score_goal, CHISEL_NPS | CHISEL_HS);
    float tech = Chisel(0.1f, 10.24f, score_goal, 0);
    float jack = Chisel(0.1f, 10.24f, score_goal, CHISEL_NPS | CHISEL_JACK);

    float techbase = max(stream, jack);
    tech *= CalcClamp(tech / techbase, 0.85f, 1.f);

    float stam;
    if (stream > tech || js > tech || hs > tech)
        if (stream > js && stream > hs)
            stam = Chisel(stream - 0.1f, 2.56f, score_goal, CHISEL_STAM | CHISEL_NPS);
        else if (js > hs)
            stam = Chisel(js - 0.1f, 2.56f, score_goal, CHISEL_STAM | CHISEL_NPS | CHISEL_JS);
        else
            stam = Chisel(hs - 0.1f, 2.56f, score_goal, CHISEL_STAM | CHISEL_NPS | CHISEL_HS);
    else
        stam = Chisel(tech - 0.1f, 2.56f, score_goal, CHISEL_STAM);

    js *= 0.95f;
    hs *= 0.95f;
    stam *= 0.9f;
    tech *= 0.95f;

    float chordjack = jack * 0.75f;

    DifficultyRating difficulty = DifficultyRating {
            0.0, // Overall rating is not set at this point
            downscale_low_accuracy_scores(stream, score_goal),
            downscale_low_accuracy_scores(js, score_goal),
            downscale_low_accuracy_scores(hs, score_goal),
            downscale_low_accuracy_scores(stam, score_goal),
            downscale_low_accuracy_scores(jack, score_goal),
            downscale_low_accuracy_scores(chordjack, score_goal),
            downscale_low_accuracy_scores(tech, score_goal)};
    
    chordjack = difficulty.handstream;
    
    difficulty.stream *= allhandsdownscaler * manyjumpsdownscaler * lotquaddownscaler;
    difficulty.jumpstream *= nojumpsdownscaler * allhandsdownscaler * lotquaddownscaler;
    difficulty.handstream *= nohandsdownscaler * allhandsdownscaler * 1.015f * manyjumpsdownscaler * lotquaddownscaler;
    difficulty.stamina *= shortstamdownscaler * 0.985f * lotquaddownscaler;
    difficulty.technical *= allhandsdownscaler * manyjumpsdownscaler * lotquaddownscaler * 1.01f;
    
    // Cap stamina to not be too far above the other skillsets
    float max_stream_jack_hs_js = max(max(difficulty.stream, difficulty.jack), max(difficulty.jumpstream, difficulty.handstream));
    difficulty.stamina = CalcClamp(difficulty.stamina, 1.f, max_stream_jack_hs_js * 1.1f);

    chordjack *= CalcClamp(qprop + hprop + jprop + 0.2f, 0.5f, 1.f) * 1.025f;

    bool downscale_chordjack_at_end = false;
    if (chordjack > difficulty.jack)
        difficulty.chordjack = chordjack;
    else
        downscale_chordjack_at_end = true;

    // fingerbias: 2.55 -> 1; 2.7 -> 0.85
    float finger_bias_scaling = CalcClamp(3.55f - fingerbias, 0.85f, 1.f);
    difficulty.technical *= finger_bias_scaling;

    if (finger_bias_scaling <= 0.95f) {
        difficulty.jack *= 1.f + (1.f - sqrt(finger_bias_scaling));
    }
    
    // If HS or JS are more prominent than stream, downscale stream a
    // little to prevent too much stream rating as a side effect from
    // JS/HS.
    // Stream is nerfed by `sqrt(hs - stream)` or `sqrt(js - stream)`
    float max_js_hs = max(difficulty.handstream, difficulty.jumpstream);
    if (difficulty.stream < max_js_hs)
        difficulty.stream -= sqrt(max_js_hs - difficulty.stream);

    // Set first overall rating
    vector<float> temp_vec = skillset_vector(difficulty);
    float overall = AggregateScores(temp_vec, 0.f, 10.24f);
    difficulty.overall = downscale_low_accuracy_scores(overall, score_goal);

    temp_vec = skillset_vector(difficulty);
    float aDvg = mean(temp_vec) * 1.2f;
    difficulty.overall = downscale_low_accuracy_scores(min(difficulty.overall, aDvg) * grindscaler, score_goal);
    difficulty.stream = downscale_low_accuracy_scores(min(difficulty.stream, aDvg * 1.0416f) * grindscaler, score_goal);
    difficulty.jumpstream = downscale_low_accuracy_scores(min(difficulty.jumpstream, aDvg * 1.0416f) * grindscaler, score_goal);
    difficulty.handstream = downscale_low_accuracy_scores(min(difficulty.handstream, aDvg) * grindscaler, score_goal);
    difficulty.stamina = downscale_low_accuracy_scores(min(difficulty.stamina, aDvg) * grindscaler, score_goal);
    difficulty.jack = downscale_low_accuracy_scores(min(difficulty.jack, aDvg) * grindscaler, score_goal);
    difficulty.chordjack = downscale_low_accuracy_scores(min(difficulty.chordjack, aDvg) * grindscaler, score_goal);
    difficulty.technical = downscale_low_accuracy_scores(min(difficulty.technical, aDvg * 1.0416f) * grindscaler, score_goal);

    difficulty.jumpstream *= jumpthrill;
    difficulty.handstream *= jumpthrill;
    difficulty.stamina *= sqrt(jumpthrill) * 0.996f;
    difficulty.technical *= sqrt(jumpthrill);

    float highest = max(difficulty.overall, highest_difficulty(difficulty));

    difficulty.overall = AggregateScores(skillset_vector(difficulty), 0.f, 10.24f);

    if (downscale_chordjack_at_end) {
        difficulty.chordjack *= 0.9f;
    }

    // Calculate and check minimum required percentage. This percentage
    // is dependant on MSD value. It's a linear function, clamped
    // between 50% and 90%. It starts at `0 MSD -> 50%` and ends at
    // `40 MSD -> 90%`
    // highest: 0 -> 50%, 40 -> 90%
    float minimum_required_percentage = CalcClamp(0.5f + (highest / 100.f), 0.f, 0.9f);
    if (score_goal < minimum_required_percentage) {
        difficulty = DifficultyRating {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    }

    // If technical is supposedly the highest skillset, but JS or HS are
    // near to it, technical might be falsely rated too high. In that
    // case downscale
    if (highest == difficulty.technical) {
        auto hs = difficulty.handstream;
        auto js = difficulty.jumpstream;
        
        // If technical within 4.5 points of HS or JS, downscale it.
        difficulty.technical -= CalcClamp(4.5f - (difficulty.technical - hs), 0.f, 4.5f);
        difficulty.technical -= CalcClamp(4.5f - (difficulty.technical - js), 0.f, 4.5f);
    }

    difficulty.jack *= 0.925f;
    difficulty.technical *= 1.025f;
    difficulty.overall = highest_difficulty(difficulty);

    return difficulty;
}

// ugly jack stuff
// Parameter j = JackSeq
// Parameter skill = player skill
float Calc::JackLoss(const vector<float>& j, float skill) {
    const float base_ceiling = 1.15f; // Jack multiplier max
    const float fscale = 1750.f; // How fast ceiling rises
    const float prop = 0.75f; // Proportion of player difficulty at which jack tax begins
    const float mag = 250.f; // Jack diff multiplier
    float output = 0.f;
    float ceiling = 1.f;
    float mod = 1.f;
    
    for (float jd : j) { // Iterate interval's jack difficulties
        // If
        // Decrease if jack difficulty is over 133% of player skill
        mod += ((jd/(prop*skill)) - 1) / mag;
        
        if (mod > 1.f)
            ceiling += (mod - 1) / fscale;
        
        mod = CalcClamp(mod, 1.f, base_ceiling * sqrt(ceiling));
        
        jd *= mod;
        
        if (skill < jd) { // If player skill below jack diffiulty
            // This can cause output to decrease if 0.96 * i < x < i
            output += 1.f - pow(skill / (jd * 0.96f), 1.5f);
        }
    }
    
    return CalcClamp(7.f * output, 0.f, 10000.f);
}

// Go through every note and determine a local jack speed difficulty at
// each place. That means:
//  1) taking the average of the most recent three note intervals,
//  2) (maybe bump that if the recent jack was really fast, i.e. minijack)
//  3) calculating 2800ms/interval_avg,
//  4) and maxing that out at the equivalent of 56 local NPS
// nps
// Returns a vector of each local jack speed difficulty
JackSeq Calc::SequenceJack(const vector<NoteInfo>& NoteInfo, unsigned int t, float music_rate) {
    vector<float> output;
    float last = -5.f;
    
    // Three most recent note intervals in ms. interval3 is the most
    // recent one.
    float interval1;
    float interval2 = 0.f;
    float interval3 = 0.f;
    
    unsigned int column = 1u << t;

    for (auto row : NoteInfo) {
        if (row.notes & column) {
            float scaledtime = row.rowTime / music_rate;
            interval1 = interval2;
            interval2 = interval3;
            interval3 = 1000.f * (scaledtime - last);
            last = scaledtime;
            
            // Take the average of last three note intervals
            float interval_avg = (interval1 + interval2 + interval3) / 3.f;
            
            // If the last interval was really fast, use that instead of
            // the average
            interval_avg = min(interval_avg, interval3 * 1.4f);
            
            // Difficulty for the 'local' jack speed
            // For example 1 NPS => 2.8; 2 NPS => 5.6; 10 NPS => 28
            float local_nps = 1000.f / interval_avg;
            float jack_difficulty = 2.8f * local_nps;
            
            // Max out local jack speed difficulty at an equivalent of 
            // ~17.857 NPS, I think
            jack_difficulty = min(jack_difficulty, 50.f);
            
            output.emplace_back(jack_difficulty);
        }
    }
    return output;
}

Finger Calc::ProcessFinger(const vector<NoteInfo>& NoteInfo, unsigned int t, float music_rate) {
    int interval_i = 0;
    float last = -5.f;
    Finger all_intervals(numitv, vector<float>());
    unsigned int column = 1u << t;

    for (size_t i = 0; i < NoteInfo.size(); i++) {
        float scaledtime = NoteInfo[i].rowTime / music_rate;

        interval_i = scaledtime / IntervalSpan;

        if (NoteInfo[i].notes & column) {
            float interval_ms = 1000 * (scaledtime - last);
            all_intervals[interval_i].emplace_back(CalcClamp(interval_ms, 40.f, 5000.f));
            last = scaledtime;
        }

        // This is only executed on the first call of this function.
        // It's hacky, and should be moved out of here, because it's not
        // part of the finger calculation process.
        if (t == 0 && NoteInfo[i].notes != 0)
            nervIntervals[interval_i].emplace_back(i);
    }
    
    return all_intervals;
}

float Calc::CalcScoreForPlayerSkill(float player_skill, ChiselFlags flags) {
    float achieved_points;
    if (flags & CHISEL_JACK) {
        // Max achievable points, minus the points the player's losing
        // from jack patterns
        achieved_points = MaxPoints
                - JackLoss(j0, player_skill)
                - JackLoss(j1, player_skill)
                - JackLoss(j2, player_skill)
                - JackLoss(j3, player_skill);
    } else {
        // Expected achieved points by left and right hand summed up
        achieved_points = left_hand.CalcInternal(player_skill, flags);
        achieved_points += right_hand.CalcInternal(player_skill, flags);
    }
    
    return achieved_points / MaxPoints;
}

// Approximate player skill required to achieve `score_goal`. The
// approximation can be influenced via the `flags`.
float Calc::Chisel(float player_skill, float resolution, float score_goal, ChiselFlags flags) {
    auto check_if_too_low = [this, flags, score_goal](float player_skill) {
        float score = CalcScoreForPlayerSkill(player_skill, flags);
        return score < score_goal;
    };
    return approximate(player_skill, resolution, 7, check_if_too_low, true);
}

// Looks at 6 smallest note intervals and returns 1375 / avg_interval_ms
// which could also be expressed as 1.375 * avg_intervals_per_second.
float Hand::CalcMSEstimate(vector<float>& input) {
    if (input.empty())
        return 0.f;

    // Sort list to be able to take the first six elements as the
    // smallest note intervals
    sort(input.begin(), input.end());
    input[0] *= 1.066f; //This is gross
    size_t length = min(input.size(), static_cast<size_t>(6));
    
    // Calculate average of `input` elements up to `length`
    float avg_interval_ms = 0; // Accumulator
    for (size_t i = 0; i < length; i++)
        avg_interval_ms += input[i];
    avg_interval_ms /= length;
    
    return 1375.f / avg_interval_ms;
}

void Hand::InitDiff(Finger& f1, Finger& f2) {
    v_itvNPSdiff = vector<float>(f1.size());
    v_itvMSdiff = vector<float>(f1.size());

    for (size_t i = 0; i < f1.size(); i++) {
        float nps_diff = 1.6f * static_cast<float>(f1[i].size() + f2[i].size());
        
        float left_ms_diff = CalcMSEstimate(f1[i]);
        float right_ms_diff = CalcMSEstimate(f2[i]);
        float ms_diff = max(left_ms_diff, right_ms_diff);
        
        v_itvNPSdiff[i] = basescaler * nps_diff;
        v_itvMSdiff[i] = basescaler * (5.f * ms_diff + 4.f * nps_diff) / 9.f;
    }
    Smooth(v_itvNPSdiff, 0.f);
    if (SmoothDifficulty)
        DifficultyMSSmooth(v_itvMSdiff);
}

// Fills in the v_itvpoints vector which holds the max number of points
// for each interval
void Hand::InitPoints(const Finger& f1, const Finger& f2) {
    for (size_t i = 0; i < f1.size(); i++)
        v_itvpoints.emplace_back(static_cast<int>(f1[i].size()) + static_cast<int>(f2[i].size()));
}

void Hand::StamAdjust(float x, vector<float>& diff) {
    float floor = 1.f;          // stamina multiplier min (increases as chart advances)
    float mod = 1.f;            // multiplier
    float last_diff = 0.f;

    for (float& i : diff) {
        // Move-average the diffs with n=2
        float diff_avg = (last_diff + i) / 2;
        last_diff = i;
        
        // Higher number -> harder to sustain this difficulty for player
        float tax = diff_avg / (prop*x);
        mod += (tax - 1) / mag;
        
        // If this section is particularly difficult, deplete stamina
        // a bit by raising the multiplier floor
        if (mod > 1.f)
            floor += (mod - 1) / fscale;
        
        // Cap and apply multiplier
        mod = CalcClamp(mod, floor, ceil);
        i *= mod;
    }
}

float Hand::CalcInternal(float player_skill, ChiselFlags flags) {
    vector<float> diff = (flags & CHISEL_NPS) ? v_itvNPSdiff : v_itvMSdiff;
    
    for (size_t i = 0; i < diff.size(); ++i) {
        diff[i] *= anchorscale[i] * rollscale[i];
        
        if (flags & CHISEL_HS) {
            diff[i] *= sqrt(ohjumpscale[i]) * jumpscale[i];
        } else if (flags & CHISEL_JS) {
            diff[i] *= hsscale[i] * hsscale[i] * sqrt(ohjumpscale[i]) * jumpscale[i];
        } else if (flags & CHISEL_NPS) {
            diff[i] *= hsscale[i] * hsscale[i] * hsscale[i] * ohjumpscale[i] * ohjumpscale[i] * jumpscale[i] * jumpscale[i];
        } else {
            diff[i] *= sqrt(ohjumpscale[i]);
        }
    }
    
    if (flags & CHISEL_STAM)
        StamAdjust(player_skill, diff);
    
    // Now, calculate the number of points the player will be expected
    // to achieve, using the individual interval's difficulties.
    
    float total_achievable_points = 0.f;
    for (size_t i = 0; i < diff.size(); i++) {
        float achievable_points = v_itvpoints[i];
        
        // If player skill below required skill for this interval
        if (player_skill <= diff[i]) {
            // Decrease the number of points the player will achieve
            achievable_points *= pow(player_skill / diff[i], 1.8f);
        }
        
        total_achievable_points += achievable_points;
    }
    return total_achievable_points;
}

vector<float> Calc::OHJumpDownscaler(const vector<NoteInfo>& NoteInfo, unsigned int firstNote, unsigned int secondNote) {
    vector<float> output;

    for (const vector<int>& interval : nervIntervals) {
        int taps = 0;
        int jumps = 0;
        for (int row : interval) {
            int columns = 0;
            if (NoteInfo[row].notes & firstNote) {
                ++columns;
            }
            if (NoteInfo[row].notes & secondNote) {
                ++columns;
            }
            if (columns == 2) {
                jumps++;
                taps += 2; //this gets added twice intentionally to mimic mina's ratings more closely
            }
            taps += columns;
        }
        if (taps == 0) {
            output.push_back(1);
        } else {
            float jump_proportion = static_cast<float>(jumps) / static_cast<float>(taps);
            // When 62.5% of taps are jumps, the downscaler will reach 0
            output.push_back(pow(1 - (1.6f * jump_proportion), 0.25f));
        }

        if (logpatterns)
            std::cout << "ohj " << output.back() << std::endl;
    }

    if (SmoothPatterns)
        Smooth(output, 1.f);
    return output;
}

// This function has an ugly side effect! It calculate `fingerbias`,
// which should definitely not be done here!
vector<float> Calc::Anchorscaler(const vector<NoteInfo>& NoteInfo, unsigned int firstNote, unsigned int secondNote) {
    vector<float> output(nervIntervals.size());

    for (size_t i = 0; i < nervIntervals.size(); i++) {
        int lcol = 0;
        int rcol = 0;
        for (int row : nervIntervals[i]) {
            if (NoteInfo[row].notes & firstNote)
                ++lcol;
            if (NoteInfo[row].notes & secondNote)
                ++rcol;
        }
        bool anyzero = lcol == 0 || rcol == 0;
        
        float smaller_col = static_cast<float>(min(lcol, rcol));
        float larger_col = static_cast<float>(max(lcol, rcol));
        
        if (anyzero) {
            output[i] = 1.f;
        } else {
            // Can range from ~0.881 (when the cols have exactly the
            // same number of notes) to approaching 1 when one column
            // has way more notes than the other.
            output[i] = CalcClamp(sqrt(1 - (smaller_col / larger_col / 4.45f)), 0.8f, 1.05f);
        }
        
        // Pls move this out of here in the future
        fingerbias += (larger_col + 2.f) / (smaller_col + 1.f);

        if (logpatterns)
            std::cout << "an " << output[i] << std::endl;
    }

    if (SmoothPatterns)
        Smooth(output, 1.f);
    return output;
}

vector<float> Calc::HSDownscaler(const vector<NoteInfo>& NoteInfo) {
    vector<float> output(nervIntervals.size());

    for (size_t i = 0; i < nervIntervals.size(); i++) {
        unsigned int taps = 0;
        unsigned int hands = 0;
        for (int row : nervIntervals[i]) {
            unsigned int notes = column_count(NoteInfo[row].notes);
            taps += notes;
            if (notes == 3)
                hands++;
        }
        if (taps == 0) {
            output[i] = 1.f;
        } else {
            // Note that this can't ever be over 1/3
            float hand_proportion = static_cast<float>(hands) / static_cast<float>(taps);
            // Therefore this downscaling value can't ever be below ~0.903
            output[i] = sqrt(sqrt(1 - hand_proportion));
        }

        if (logpatterns)
            std::cout << "hs " << output[i] << std::endl;
    }

    if (SmoothPatterns)
        Smooth(output, 1.f);
    return output;
}

vector<float> Calc::JumpDownscaler(const vector<NoteInfo>& NoteInfo) {
    vector<float> output(nervIntervals.size());

    for (size_t i = 0; i < nervIntervals.size(); i++) {
        unsigned int taps = 0;
        unsigned int jumps = 0;
        for (int row : nervIntervals[i]) {
            unsigned int notes = column_count(NoteInfo[row].notes);
            taps += notes;
            if (notes == 2)
                jumps++;
        }
        if (taps == 0) {
            output[i] = 1.f;
        } else {
            // Note that this can't ever be over 1/2
            float jump_proportion = static_cast<float>(jumps) / static_cast<float>(taps);
            // Therefore this downscaling value can't ever be below ~0.955
            output[i] = sqrt(sqrt(1 - jump_proportion / 3.f));
        }

        if (logpatterns)
            std::cout << "ju " << output[i] << std::endl;
    }
    if (SmoothPatterns)
        Smooth(output, 1.f);
    return output;
}


vector<float> Calc::RollDownscaler(const Finger& f1, const Finger& f2) {
    vector<float> output(f1.size());    //this is slightly problematic because if one finger is longer than the other
                                        //you could potentially have different results with f1 and f2 switched
    for (size_t i = 0; i < f1.size(); i++) {
        // If there is none or only one note in this interval, skip
        if (f1[i].size() + f2[i].size() <= 1) {
            output[i] = 1.f;
            continue;
        }
        vector<float> hand_intervals;
        for (float time1 : f1[i])
            hand_intervals.emplace_back(time1);
        for (float time2 : f2[i])
            hand_intervals.emplace_back(time2);

        float interval_mean = mean(hand_intervals);

        for (float & note : hand_intervals)
            if (interval_mean / note < 0.6f)
                note = interval_mean;

        float interval_cv = cv(hand_intervals) + 0.85f;
        output[i] = interval_cv >= 1.0f ? min(sqrt(sqrt(interval_cv)), 1.075f) : interval_cv*interval_cv*interval_cv;

        if (logpatterns)
            std::cout << "ro " << output[i] << std::endl;
    }

    if (SmoothPatterns)
        Smooth(output, 1.f);

    return output;
}

// Function to generate SSR rating
DifficultyRating MinaSDCalc(const vector<NoteInfo>& NoteInfo, float musicrate, float goal) {
    if (NoteInfo.empty()) {
        return DifficultyRating {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0};
    }
    return std::make_unique<Calc>()->CalcMain(NoteInfo, musicrate, goal);
}

// Wrap difficulty calculation for all rates from 0.7 to 2.1, with 0.1
// step
MinaSD MinaSDCalc(const vector<NoteInfo>& NoteInfo) {
    MinaSD allrates;
    int lower_rate = 7;
    int upper_rate = 21;

    if (!NoteInfo.empty())
        for (int i = lower_rate; i < upper_rate; i++)
            allrates.emplace_back(MinaSDCalc(NoteInfo, static_cast<float>(i) / 10.f, 0.93f));
    else {
        DifficultyRating output{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        for (int i = lower_rate; i < upper_rate; i++)
            allrates.emplace_back(output);
    }
    return allrates;
}

int GetCalcVersion()
{
    return -1;
}
