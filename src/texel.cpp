/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>

  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef TUNE

#include <math.h>
#include <cstdlib>
#include <cstring>

#include "bitboards.h"
#include "board.h"
#include "evaluate.h"
#include "history.h"
#include "move.h"
#include "search.h"
#include "texel.h"
#include "thread.h"
#include "transposition.h"
#include "types.h"
#include "uci.h"
#include "zobrist.h"

// Internal Memory Managment
TexelTuple* TupleStack;
int TupleStackSize = STACKSIZE;

// Tap into evaluate()
extern EvalTrace T, EmptyTrace;

extern const int PawnValue;
extern const int KnightValue;
extern const int BishopValue;
extern const int RookValue;
extern const int QueenValue;
extern const int KingValue;
extern const int PawnPSQT32[32];
extern const int KnightPSQT32[32];
extern const int BishopPSQT32[32];
extern const int RookPSQT32[32];
extern const int QueenPSQT32[32];
extern const int KingPSQT32[32];
extern const int PawnCandidatePasser[2][8];
extern const int PawnIsolated;
extern const int PawnStacked[2];
extern const int PawnBackwards[2];
extern const int PawnConnected32[32];
extern const int KnightOutpost[2][2];
extern const int KnightBehindPawn;
extern const int ClosednessKnightAdjustment[9];
extern const int KnightMobility[9];
extern const int BishopPair;
extern const int BishopRammedPawns;
extern const int BishopOutpost[2][2];
extern const int BishopBehindPawn;
extern const int BishopMobility[14];
extern const int RookFile[2];
extern const int RookOnSeventh;
extern const int ClosednessRookAdjustment[9];
extern const int RookMobility[15];
extern const int QueenMobility[28];
extern const int KingDefenders[12];
extern const int KingPawnFileProximity[8];
extern const int KingShelter[2][8][8];
extern const int KingStorm[2][4][8];
extern const int PassedPawn[2][2][8];
extern const int PassedFriendlyDistance[8];
extern const int PassedEnemyDistance[8];
extern const int PassedSafePromotionPath;
extern const int ThreatWeakPawn;
extern const int ThreatMinorAttackedByPawn;
extern const int ThreatMinorAttackedByMinor;
extern const int ThreatMinorAttackedByMajor;
extern const int ThreatRookAttackedByLesser;
extern const int ThreatMinorAttackedByKing;
extern const int ThreatRookAttackedByKing;
extern const int ThreatQueenAttackedByOne;
extern const int ThreatOverloadedPieces;
extern const int ThreatByPawnPush;
extern const int ComplexityTotalPawns;
extern const int ComplexityPawnFlanks;
extern const int ComplexityPawnEndgame;
extern const int ComplexityAdjustment;

void runTexelTuning(Thread *thread) {

    TexelEntry *tes;
    int iteration = -1;
    double K, error, best = 1e6, rate = LEARNING;
    TexelVector params = {0}, cparams = {0}, phases = {0};

    setvbuf(stdout, nullptr, _IONBF, 0);

    cout << "\nTUNER WILL BE TUNING " << NTERMS << " TERMS...";

    cout << "\n\nSETTING TABLE SIZE TO 1MB FOR SPEED...";
    initTT(1);

    cout << "\n\nALLOCATING MEMORY FOR TEXEL ENTRIES [" << (int)(NPOSITIONS * sizeof(TexelEntry) / (1024 * 1024)) << "MB]...";
    tes = calloc(NPOSITIONS, sizeof(TexelEntry));

    cout << "\n\nALLOCATING MEMORY FOR TEXEL TUPLE STACK [" << (int)(STACKSIZE * sizeof(TexelTuple) / (1024 * 1024)) << "MB]...";
    TupleStack = calloc(STACKSIZE, sizeof(TexelTuple));

    cout << "\n\nINITIALIZING TEXEL ENTRIES FROM FENS...";
    initTexelEntries(tes, thread);

    cout << "\n\nFETCHING CURRENT EVALUATION TERMS AS A STARTING POINT...";
    initCurrentParameters(cparams);

    cout << "\n\nSETTING TERM PHASES, MG, EG, OR BOTH...";
    initPhaseManager(phases);

    cout << "\n\nCOMPUTING OPTIMAL K VALUE...\n";
    K = computeOptimalK(tes);

    while (1) {

        // Shuffle the dataset before each epoch
        if (NPOSITIONS != BATCHSIZE)
            shuffleTexelEntries(tes);

        // Report every REPORTING iterations
        if (++iteration % REPORTING == 0) {

            // Check for a regression in tuning
            error = completeLinearError(tes, params, K);
            if (error > best) rate = rate / LRDROPRATE;

            // Report current best parameters
            best = error;
            printParameters(params, cparams);
            cout << "\nIteration [" << iteration << "] Error = " << best << " \n";
        }

        for (int batch = 0; batch < NPOSITIONS / BATCHSIZE; ++batch) {

            TexelVector gradient = {0};
            updateGradient(tes, gradient, params, phases, K, batch);

            // Update Parameters. Note that in updateGradient() we skip the multiplcation by negative
            // two over BATCHSIZE. This is done only here, just once, for precision and a speed gain
            for (int i = 0; i < NTERMS; ++i)
                for (int j = MG; j <= EG; ++j)
                    params[i][j] += (2.0 / BATCHSIZE) * rate * gradient[i][j];
        }
    }
}

void initTexelEntries(TexelEntry *tes, Thread *thread) {

    Undo undo[1];
    Limits limits;
    char line[128];
    int i, j, k, searchEval, coeffs[NTERMS];
    FILE *fin = fopen("FENS", "r");

    // Initialize the thread for the search
    thread->limits = &limits; thread->depth  = 0;

    // Create a TexelEntry for each FEN
    for (i = 0; i < NPOSITIONS; ++i) {

        // Read next position from the FEN file
        if (fgets(line, 128, fin) == nullptr) {
            cout << "Unable to read line #" << i << "\n";
            exit(EXIT_FAILURE);
        }

        // Occasional reporting for total completion
        if ((i + 1) % 10000 == 0 || i == NPOSITIONS - 1)
            cout << "\rINITIALIZING TEXEL ENTRIES FROM FENS...  [" << i + 1 << "d OF " << NPOSITIONS << "d]";

        // Fetch and cap a white POV search
        searchEval = atoi(strstr(line, "] ") + 2);
        if (strstr(line, " b ")) searchEval *= -1;

        // Determine the result of the game
        if      (strstr(line, "[1.0]")) tes[i].result = 1.0;
        else if (strstr(line, "[0.0]")) tes[i].result = 0.0;
        else if (strstr(line, "[0.5]")) tes[i].result = 0.5;
        else    {cout << "Cannot Parse " << line); exit(EXIT_FAILURE << "\n";}

        // Resolve FEN to a quiet position
        boardFromFEN(thread->board, line, 0);
        qsearch(thread, &thread->pv, -MATE, MATE, 0);
        for (j = 0; j < thread->pv.length; ++j)
            applyMove(&thread->board, thread->pv.line[j], undo);

        // Determine the game phase based on remaining material
        tes[i].phase = 24 - 4 * popcount(thread->board.pieces[QUEEN ])
                          - 2 * popcount(thread->board.pieces[ROOK  ])
                          - 1 * popcount(thread->board.pieces[BISHOP])
                          - 1 * popcount(thread->board.pieces[KNIGHT]);

        // Compute phase factors for updating the gradients and
        tes[i].factors[MG] = 1 - tes[i].phase / 24.0;
        tes[i].factors[EG] = 0 + tes[i].phase / 24.0;

        // Finish the phase calculation for the evaluation
        tes[i].phase = (tes[i].phase * 256 + 12) / 24.0;

        // Vectorize the evaluation coefficients and save the eval
        // relative to WHITE. We must first clear the coeff vector.
        T = EmptyTrace;
        tes[i].eval = evaluateBoard(&thread->board, nullptr);
        if (thread->board.turn == BLACK) tes[i].eval *= -1;
        initCoefficients(coeffs);

        // Weight the Static and Search evals
        tes[i].eval = tes[i].eval * STATICWEIGHT
                    +  searchEval * SEARCHWEIGHT;

        // Count up the non zero coefficients
        for (k = 0, j = 0; j < NTERMS; ++j)
            k += coeffs[j] != 0;

        // Allocate Tuples
        updateMemory(&tes[i], k);

        // Initialize the Texel Tuples
        for (k = 0, j = 0; j < NTERMS; ++j) {
            if (coeffs[j] != 0){
                tes[i].tuples[k].index = j;
                tes[i].tuples[k++].coeff = coeffs[j];
            }
        }
    }

    fclose(fin);
}

void initCoefficients(int coeffs[NTERMS]) {

    int i = 0; // EXECUTE_ON_TERMS will update i accordingly

    EXECUTE_ON_TERMS(INIT_COEFF);

    if (i != NTERMS){
        cout << "Error in initCoefficients(): i = " << i << " ; NTERMS = " << NTERMS << "\n";
        exit(EXIT_FAILURE);
    }
}

void initCurrentParameters(TexelVector cparams) {

    int i = 0; // EXECUTE_ON_TERMS will update i accordingly

    EXECUTE_ON_TERMS(INIT_PARAM);

    if (i != NTERMS){
        cout << "Error in initCurrentParameters(): i = " << i << " ; NTERMS = " << NTERMS << "\n";
        exit(EXIT_FAILURE);
    }
}

void initPhaseManager(TexelVector phases) {

    int i = 0; // EXECUTE_ON_TERMS will update i accordingly

    EXECUTE_ON_TERMS(INIT_PHASE);

    if (i != NTERMS){
        cout << "Error in initPhaseManager(): i = " << i << " ; NTERMS = " << NTERMS << "\n";
        exit(EXIT_FAILURE);
    }
}

void updateMemory(TexelEntry *te, int size) {

    // First ensure we have enough Tuples left for this TexelEntry
    if (size > TupleStackSize) {
        cout << "\n\nALLOCATING MEMORY FOR TEXEL TUPLE STACK [" << (int)(STACKSIZE * sizeof(TexelTuple) / (1024 * 1024)) << "MB]...\n\n";
        TupleStackSize = STACKSIZE;
        TupleStack = calloc(STACKSIZE, sizeof(TexelTuple));
    }

    // Allocate Tuples for the given TexelEntry
    te->tuples = TupleStack;
    te->ntuples = size;

    // Update internal memory manager
    TupleStack += size;
    TupleStackSize -= size;
}

void updateGradient(TexelEntry *tes, TexelVector gradient, TexelVector params, TexelVector phases, double K, int batch) {

    #pragma omp parallel shared(gradient)
    {
        TexelVector local = {0};
        #pragma omp for schedule(static, BATCHSIZE / NPARTITIONS)
        for (int i = batch * BATCHSIZE; i < (batch + 1) * BATCHSIZE; ++i) {

            double error = singleLinearError(&tes[i], params, K);

            for (int j = 0; j < tes[i].ntuples; ++j)
                for (int k = MG; k <= EG; ++k)
                    local[tes[i].tuples[j].index][k] += error * tes[i].factors[k] * tes[i].tuples[j].coeff;
        }

        for (int i = 0; i < NTERMS; ++i)
            for (int j = MG; j <= EG; ++j)
                if (phases[i][j]) gradient[i][j] += local[i][j];
    }
}

void shuffleTexelEntries(TexelEntry *tes) {

    for (int i = 0; i < NPOSITIONS; ++i) {

        int A = rand64() % NPOSITIONS;
        int B = rand64() % NPOSITIONS;

        TexelEntry temp = tes[A];
        tes[A] = tes[B];
        tes[B] = temp;
    }
}

double computeOptimalK(TexelEntry *tes) {

    double start = -10.0, end = 10.0, delta = 1.0;
    double curr = start, error, best = completeEvaluationError(tes, start);

    for (int i = 0; i < KPRECISION; ++i) {

        curr = start - delta;
        while (curr < end) {
            curr = curr + delta;
            error = completeEvaluationError(tes, curr);
            if (error <= best)
                best = error, start = curr;
        }

        cout << "COMPUTING K ITERATION [" << i << "] K = " << start << " E = " << best << "\n";

        end = start + delta;
        start = start - delta;
        delta = delta / 10.0;
    }

    return start;
}

double completeEvaluationError(TexelEntry *tes, double K) {

    double total = 0.0;

    #pragma omp parallel shared(total)
    {
        #pragma omp for schedule(static, NPOSITIONS / NPARTITIONS) reduction(+:total)
        for (int i = 0; i < NPOSITIONS; ++i)
            total += pow(tes[i].result - sigmoid(K, tes[i].eval), 2);
    }

    return total / (double)NPOSITIONS;
}

double completeLinearError(TexelEntry *tes, TexelVector params, double K) {

    double total = 0.0;

    #pragma omp parallel shared(total)
    {
        #pragma omp for schedule(static, NPOSITIONS / NPARTITIONS) reduction(+:total)
        for (int i = 0; i < NPOSITIONS; ++i)
            total += pow(tes[i].result - sigmoid(K, linearEvaluation(&tes[i], params)), 2);
    }

    return total / (double)NPOSITIONS;
}

double singleLinearError(TexelEntry *te, TexelVector params, double K) {
    double sigm = sigmoid(K, linearEvaluation(te, params));
    double sigmprime = sigm * (1 - sigm);
    return (te->result - sigm) * sigmprime;
}

double linearEvaluation(TexelEntry *te, TexelVector params) {

    double mg = 0, eg = 0;

    for (int i = 0; i < te->ntuples; ++i) {
        mg += te->tuples[i].coeff * params[te->tuples[i].index][MG];
        eg += te->tuples[i].coeff * params[te->tuples[i].index][EG];
    }

    return te->eval + ((mg * (256 - te->phase) + eg * te->phase) / 256.0);
}

double sigmoid(double K, double S) {
    return 1.0 / (1.0 + pow(10.0, -K * S / 400.0));
}

void printParameters(TexelVector params, TexelVector cparams) {

    int tparams[NTERMS][PHASE_NB];

    // Combine updated and current parameters
    for (int j = 0; j < NTERMS; ++j) {
        tparams[j][MG] = round(params[j][MG] + cparams[j][MG]);
        tparams[j][EG] = round(params[j][EG] + cparams[j][EG]);
    }

    int i = 0; // EXECUTE_ON_TERMS will update i accordingly

    EXECUTE_ON_TERMS(PRINT_PARAM);

    if (i != NTERMS) {
        cout << "Error in printParameters(): i = " << i << " ; NTERMS = " << NTERMS << "\n";
        exit(EXIT_FAILURE);
    }
}

void printParameters_0(char *name, int params[NTERMS][PHASE_NB], int i) {
    cout << "const int " << name << " = S(" << params[i][MG] << "d," << params[i][EG] << "d);\n\n";
    i++;
}

void printParameters_1(char *name, int params[NTERMS][PHASE_NB], int i, int A) {

    cout << "const int " << name << "[" << A << "] = {";

    for (int a = 0; a < A; a++, i++) {
        if (a % 4 == 0) cout << "\n    ";
        cout << "S(" << params[i][MG] << "d," << params[i][EG] << "d), ";
    }

    cout << "\n};\n\n";
}

void printParameters_2(char *name, int params[NTERMS][PHASE_NB], int i, int A, int B) {

    cout << "const int " << name << "[" << A << "][" << B << "] = {\n";

    for (int a = 0; a < A; ++a) {

        cout << "   {";

        for (int b = 0; b < B; b++, i++) {
            cout << "S(" << params[i][MG] << "d," << params[i][EG] << "d)";
            cout << "%s, b == B - 1 ?  : , "
        }

        cout << "},\n";
    }

    cout << "};\n\n";

}

void printParameters_3(char *name, int params[NTERMS][PHASE_NB], int i, int A, int B, int C) {

    cout << "const int " << name << "[" << A << "][" << B << "][" << C << "] = {\n";

    for (int a = 0; a < A; ++a) {

        for (int b = 0; b < B; ++b) {

            cout << "%s, b ?    { :   {{";

            for (int c = 0; c < C; c++, i++) {
                cout << "S(" << params[i][MG] << "d," << params[i][EG] << "d)";
                cout << "%s, c == C - 1 ?  : , "
            }

            cout << "%s, b == B - 1 ? }},\n : },\n"
        }

    }

    cout << "};\n\n";
}

#endif