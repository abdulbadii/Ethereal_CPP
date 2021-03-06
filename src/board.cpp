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

#include <cassert>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

#include "attacks.h"
#include "bitboards.h"
#include "board.h"
#include "evaluate.h"
#include "masks.h"
#include "move.h"
#include "movegen.h"
#include "search.h"
#include "thread.h"
#include "time.h"
#include "transposition.h"
#include "types.h"
#include "uci.h"
#include "zobrist.h"

#include <iostream>
using namespace std;
const char *PieceLabel[COLOUR_NB] = {"PNBRQK", "pnbrqk"};

namespace {
const string Benchmarks[] = {
	#include "bench.csv"
	""
};
void setSquare(Board& board, int colour, int piece, int sq) {

	// Generate a piece on the given square. This serves as an aid
	// to setting up the board from a FEN. We make sure update any
	// related hash values, as well as the PSQT + material values

	assert(0 <= colour && colour < COLOUR_NB);
	assert(0 <= piece && piece < PIECE_NB);
	assert(0 <= sq && sq < SQUARE_NB);

	board.squares[sq] = makePiece(piece, colour);
	setBit(board.colours[colour], sq);
	setBit(board.pieces[piece], sq);

	board.psqtmat += PSQT[board.squares[sq]][sq];
	board.hash ^= ZobristKeys[board.squares[sq]][sq];
	if (piece == PAWN || piece == KING)
		board.pkhash ^= ZobristKeys[board.squares[sq]][sq];
}
int stringToSquare(string& str) {

	// Helper for reading the enpass square from a FEN. If no square
	// is provided, Ethereal will use -1 to represent this internally

	return str[0] == '-' ? -1 : square(str[1] - '1', str[0] - 'a');
}
}

void squareToString(int sq, char *str) {

	// Helper for writing the enpass square, as well as for converting
	// a move into long algabraic notation. When there is not an enpass
	// square we will output a "-" as expected for a FEN

	assert(-1 <= sq && sq < SQUARE_NB);

	if (sq == -1)
		*str++ = '-';
	else {
		*str++ = fileOf(sq) + 'a';
		*str++ = rankOf(sq) + '1';
	}

	*str = 0;
}

void boardFromFEN(Board& board,const string& fens, int chess960) {

	static const uint64_t StandardCastles = (1ull <<  0) | (1ull <<  7)
														| (1ull << 56) | (1ull << 63);

	int sq = 56;
	char ch;
	string word(43,0), fen=fens;
	uint64_t rooks, kings, white, black;

	board(); // Zero out, set squares to EMPTY

	parse(fen, word);
	uint16_t i=0;
	// Piece placement
	while ((ch = word[i++])) {
		if (isdigit(ch))
				sq += ch - '0';
		else if (ch == '/')
				sq -= 16;
		else {
				const bool colour = islower(ch);
				const char *piece = strchr(PieceLabel[colour], ch);

				if (piece)
					setSquare(board, colour, piece - PieceLabel[colour], sq++);
		}
	}

	// Turn of play
	parse(fen, word);
	board.turn = word[0] == 'w' ? WHITE : BLACK;
	if (board.turn == BLACK) board.hash ^= ZobristTurnKey;

	// Castling rights
	parse(fen, word);

	rooks = board.pieces[ROOK];
	kings = board.pieces[KING];
	white = board.colours[WHITE];
	black = board.colours[BLACK];
	i=0;
	while ((ch = word[i++])) {
		switch(ch) {
		case 'K': setBit(board.castleRooks, getmsb(white & rooks & RANK_1)); break;
		case 'Q': setBit(board.castleRooks, getlsb(white & rooks & RANK_1));  break;
		case 'k': setBit(board.castleRooks, getmsb(black & rooks & RANK_8)); break;
		case 'q': setBit(board.castleRooks, getlsb(black & rooks & RANK_8));   break;
		default:
		if ('A' <= ch && ch <= 'H') setBit(board.castleRooks, square(0, ch - 'A'));
		else if ('a' <= ch && ch <= 'h') setBit(board.castleRooks, square(7, ch - 'a'));
		break;
		}
	}

	for (sq = 0; sq < SQUARE_NB; ++sq) {
		board.castleMasks[sq] = allON;
		if (testBit(board.castleRooks, sq)) clearBit(board.castleMasks[sq], sq);
		if (testBit(white & kings, sq)) board.castleMasks[sq] &= ~white;
		if (testBit(black & kings, sq)) board.castleMasks[sq] &= ~black;
	}

	rooks = board.castleRooks;
	while (rooks) board.hash ^= ZobristCastleKeys[poplsb(rooks)];

	// En passant square
	board.epSquare = stringToSquare(parse(fen, word));
	if (board.epSquare != -1)
		board.hash ^= ZobristEnpassKeys[fileOf(board.epSquare)];

	// Half & Full Move Counters
	board.halfMoveCounter = stoi(parse(fen, word));
	board.fullMoveCounter = stoi(parse(fen, word));

	// Move count: ignore and use zero, as we count since root
	board.numMoves = 0;

	// Need king attackers for move generation
	board.kingAttackers = attackersToKingSquare(board);

	// We save the game mode in order to comply with the UCI rules for printing
	// moves. If chess960 is not enabled, but we have detected an unconventional
	// castle setup, then we set chess960 to be true on our own. Currently, this
	// is simply a hack so that FRC positions may be added to the bench.csv
	board.chess960 = chess960 || (board.castleRooks & ~StandardCastles);

}

void boardToFEN(Board& board, string& fen) {

	int sq;
	char str[3];
	uint64_t castles;
	uint16_t i=0, cnt;

	// Piece placement
	for (int r = RANK_NB-1; r >= 0; --r) {
		cnt = 0;

		for (int f = 0; f < FILE_NB; ++f) {
				const int s = square(r, f);
				const int p = board.squares[s];

				if (p != EMPTY) {
					if (cnt)
						fen[i++] = cnt + '0';

					fen[i++] = PieceLabel[pieceColour(p)][pieceType(p)];
					cnt = 0;
				} else
					cnt++;
		}

		if (cnt)
				fen[i++] = cnt + '0';

		fen[i++] = r == 0 ? ' ' : '/';
	}

	// Turn of play
	fen[i++] = board.turn == WHITE ? 'w' : 'b';
	fen[i++] = ' ';

	// Castle rights for White
	castles = board.colours[WHITE] & board.castleRooks;
	while (castles) {
		sq = popmsb(castles);
		if (board.chess960) fen[i++] = 'A' + fileOf(sq);
		else if (testBit(FILE_H, sq)) fen[i++] = 'K';
		else if (testBit(FILE_A, sq)) fen[i++] = 'Q';
	}

	// Castle rights for Black
	castles = board.colours[BLACK] & board.castleRooks;
	while (castles) {
		sq = popmsb(castles);
		if (board.chess960) fen[i++] = 'a' + fileOf(sq);
		else if (testBit(FILE_H, sq)) fen[i++] = 'k';
		else if (testBit(FILE_A, sq)) fen[i++] = 'q';
	}

	// Check for empty Castle rights
	if (!board.castleRooks)
		fen[i++] = '-';

	// En passant square, Half Move Counter, and Full Move Counter
	squareToString(board.epSquare, str);
	sprintf(&fen[0], " %s %d %d", str, board.halfMoveCounter, board.fullMoveCounter);
}

void printBoard(Board& board) {

	string fen(256,0);

	// Print each row of the board, starting from the top
	for(int sq = square(RANK_NB-1, 0); sq >= 0; sq -= FILE_NB) {

		cout << "\n     |----|----|----|----|----|----|----|----|\n";
		cout << "   " << 1 + sq / 8 << " ";

		// Print each square in a row, starting from the left
		for(int i = 0; i < 8; ++i) {

				int colour = pieceColour(board.squares[sq+i]);
				int type = pieceType(board.squares[sq+i]);

				switch(colour){
					case WHITE: cout << "| *" << PieceLabel[colour][type] << " "; break;
					case BLACK: cout << "|  " << PieceLabel[colour][type] << " "; break;
					default   : cout << "|    "; break;
				}
		}

		cout << "|";
	}

	cout << "\n     |----|----|----|----|----|----|----|----|";
	cout << "\n        A    B    C    D    E    F    G    H\n";

	// Print FEN
	boardToFEN(board, fen);
	cout << "\n" << fen << "\n\n";
}

int boardHasNonPawnMaterial(Board& board, int turn) {
	uint64_t friendly = board.colours[turn];
	uint64_t kings = board.pieces[KING];
	uint64_t pawns = board.pieces[PAWN];
	return (friendly & (kings | pawns)) != friendly;
}

int boardIsDrawn(Board& board, int height) {

	// Drawn if any of the three possible cases
	return boardDrawnByFiftyMoveRule(board)
		|| boardDrawnByRepetition(board, height)
		|| boardDrawnByInsufficientMaterial(board);
}

int boardDrawnByFiftyMoveRule(Board& board) {

	// Fifty move rule triggered. BUG: We do not account for the case
	// when the fifty move rule occurs as checkmate is delivered, which
	// should not be considered a drawn position, but a checkmated one.
	return board.halfMoveCounter > 99;
}

int boardDrawnByRepetition(Board& board, int height) {

	int reps = 0;

	// Look through hash histories for our moves
	for (int i = board.numMoves - 2; i >= 0; i -= 2) {

		// No draw can occur before a zeroing move
		if (i < board.numMoves - board.halfMoveCounter)
				break;

		// Check for matching hash with a two fold after the root,
		// or a three fold which occurs in part before the root move
		if (    board.history[i] == board.hash
				&& (i > board.numMoves - height || ++reps == 2))
				return 1;
	}

	return 0;
}

int boardDrawnByInsufficientMaterial(Board& board) {

	// Check for KvK, KvN, KvB, and KvNN.

	return !(board.pieces[PAWN] | board.pieces[ROOK] | board.pieces[QUEEN])
		&& (!several(board.colours[WHITE]) || !several(board.colours[BLACK]))
		&& (    !several(board.pieces[KNIGHT] | board.pieces[BISHOP])
				|| (!board.pieces[BISHOP] && popcount(board.pieces[KNIGHT]) <= 2));
}

uint64_t perft(Board& board, int depth) {

	Undo undo;
	int size = 0;
	uint64_t found = 0ull;
	uint16_t moves[MAX_MOVES];

	if (depth == 0)
		return 1ull;

	genAllNoisyMoves(board, moves, size);
	genAllQuietMoves(board, moves, size);

	// Recurse on all valid moves
	for(size -= 1; size >= 0; --size) {
		applyMove(board, moves[size], undo);
		if (moveWasLegal(board)) found += perft(board, depth-1);
		revertMove(board, moves[size], undo);
	}

	return found;
}

void runBenchmark(int argc, char** argv) {

	Board board;
	Limits limits;
	Thread *threads;

	double start;
	uint64_t nodes = 0ull;
	uint16_t bestMove, ponderMove;

	int depth     = argc > 2 ? atoi(argv[2]) : 13;
	int nthreads  = argc > 3 ? atoi(argv[3]) : 1;
	int megabytes = argc > 4 ? atoi(argv[4]) : 16;

	initTT(megabytes);
	threads = createThreadPool(nthreads);

	// Initialize a "go depth <x>" search
	limits.limitedByNone  = 0;
	limits.limitedByTime  = 0;
	limits.limitedByDepth = 1;
	limits.limitedBySelf  = 0;
	limits.timeLimit      = 0;
	limits.depthLimit     = depth;
	limits.multiPV        = 1;

	start = getRealTime();

	for (int i = 0; Benchmarks[i].size(); ++i) {
		cout << "\nPosition #" << i + 1 << ": " << Benchmarks[i] << "\n";
		boardFromFEN(board, Benchmarks[i], 0);
		limits.start = getRealTime();
		getBestMove(threads, board, limits, bestMove, ponderMove);
		nodes += nodesSearchedThreadPool(threads);
		clearTT(); // Reset TT for new search
	}

	cout << "Time  : " << int(getRealTime() - start) << "ms\n";
	cout << "Nodes : " << nodes << "\n";
	cout << "NPS   : " << int(nodes / ((getRealTime() - start) / 1000.0)) << "\n";

	free(threads);
}