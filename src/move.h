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

#pragma once

#include <cstdint>

#include "types.h"
#include "thread.h"
#include "movegen.h"

enum {
    NONE_MOVE = 0, NULL_MOVE = 11,

    NORMAL_MOVE = 0 << 12, CASTLE_MOVE    = 1 << 12,
    ENPASS_MOVE = 2 << 12, PROMOTION_MOVE = 3 << 12,

    PROMOTE_TO_KNIGHT = 0 << 14, PROMOTE_TO_BISHOP = 1 << 14,
    PROMOTE_TO_ROOK   = 2 << 14, PROMOTE_TO_QUEEN  = 3 << 14,

    KNIGHT_PROMO_MOVE = PROMOTION_MOVE | PROMOTE_TO_KNIGHT,
    BISHOP_PROMO_MOVE = PROMOTION_MOVE | PROMOTE_TO_BISHOP,
    ROOK_PROMO_MOVE   = PROMOTION_MOVE | PROMOTE_TO_ROOK,
    QUEEN_PROMO_MOVE  = PROMOTION_MOVE | PROMOTE_TO_QUEEN
};

int castleKingTo(int king, int rook);
int castleRookTo(int king, int rook);

#define MoveFrom(move)         (((move) >> 0) & 63)
#define MoveTo(move)           (((move) >> 6) & 63)
#define MoveType(move)         ((move) & (3 << 12))
#define MovePromoType(move)    ((move) & (3 << 14))
#define MovePromoPiece(move)   (1 + ((move) >> 14))
#define MoveMake(from,to,flag) ((from) | ((to) << 6) | (flag))

// int apply(Thread *thread, Board& board, uint16_t move, int height);
// void applyLegal(Thread *thread, Board& board, uint16_t move, int height);
void applyMove(Board& board, uint16_t move, Undo& undo);
void applyNormalMove(Board& board, uint16_t move, Undo& undo);
void applyCastleMove(Board& board, uint16_t move, Undo& undo);
void applyEnpassMove(Board& board, uint16_t move, Undo& undo);
void applyPromotionMove(Board& board, uint16_t move, Undo& undo);
void applyNullMove(Board& board, Undo& undo);

// void revert(Thread *thread, Board& board, uint16_t move, int height);
// void revertNullMove(Board& board, Undo& undo);
void revertMove(Board& board, uint16_t move, Undo& undo);

int legalMoveCount(Board& board);
int moveExaminedByMultiPV(Thread *thread, uint16_t move);
int moveIsTactical(const Board& board, uint16_t move);
int moveEstimatedValue(const Board& board, uint16_t move);
int moveBestCaseValue(const Board& board);
int moveIsPseudoLegal(const Board& board, uint16_t move);
int moveWasLegal(const Board& board);
void moveToString(uint16_t move, char *str, int chess960);


inline int apply(Thread *thread, Board& board, uint16_t move, int height) {

	// nul moves are only tried when legal
	if (move == NULL_MOVE) {
		thread->moveStack[height] = NULL_MOVE;
		applyNullMove(board, thread->undoStack[height]);
		return 1;
	}

	// Track some move information for history lookups
	thread->moveStack[height] = move;
	thread->pieceStack[height] = pieceType(board.squares[MoveFrom(move)]);

	// Apply the move and reject if illegal
	applyMove(board, move, thread->undoStack[height]);
	if (!moveWasLegal(board))
		return revertMove(board, move, thread->undoStack[height]), 0;

	return 1;
}

inline void applyLegal(Thread *thread, Board& board, uint16_t move, int height) {

	// Track some move information for history lookups
	thread->moveStack[height] = move;
	thread->pieceStack[height] = pieceType(board.squares[MoveFrom(move)]);

	// Assumed that this move is legal
	applyMove(board, move, thread->undoStack[height]);
	assert(moveWasLegal(board));
}

inline void revertNullMove(Board& board, Undo& undo) {

	// Revert information which is hard to recompute
	// We may, and have to, zero out the king attacks
	board.hash            = undo.hash;
	board.kingAttackers   = 0ull;
	board.epSquare        = undo.epSquare;
	board.halfMoveCounter = undo.halfMoveCounter;

	// nullptr moves simply swap the turn only
	board.turn = !board.turn;
	board.numMoves--;
}

inline void revert(Thread *thread, Board& board, uint16_t move, int height) {
	if (move == NULL_MOVE) revertNullMove(board, thread->undoStack[height]);
	else revertMove(board, move, thread->undoStack[height]);
}
inline int legalMoveCount(Board& board) {

	// Count of the legal number of moves for a given position

	int size = 0;
	uint16_t moves[MAX_MOVES];
	genAllLegalMoves(board, moves, size);

	return size;
}

