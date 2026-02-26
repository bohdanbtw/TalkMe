#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace TalkMe {

struct ChessMove {
    int fromRow, fromCol, toRow, toCol;
    char promoted = 0;
};

class ChessEngine {
public:
    ChessEngine() { Reset(); }

    void Reset() {
        const char init[8][8] = {
            {'r','n','b','q','k','b','n','r'},
            {'p','p','p','p','p','p','p','p'},
            {' ',' ',' ',' ',' ',' ',' ',' '},
            {' ',' ',' ',' ',' ',' ',' ',' '},
            {' ',' ',' ',' ',' ',' ',' ',' '},
            {' ',' ',' ',' ',' ',' ',' ',' '},
            {'P','P','P','P','P','P','P','P'},
            {'R','N','B','Q','K','B','N','R'}
        };
        std::memcpy(board, init, sizeof(init));
        whiteToMove = true;
        whiteKingMoved = blackKingMoved = false;
        whiteRookAMoved = whiteRookHMoved = false;
        blackRookAMoved = blackRookHMoved = false;
        enPassantCol = -1;
        gameOver = false;
        result.clear();
    }

    bool IsLegalMove(int fr, int fc, int tr, int tc) const {
        if (fr < 0 || fr > 7 || fc < 0 || fc > 7 || tr < 0 || tr > 7 || tc < 0 || tc > 7) return false;
        char piece = board[fr][fc];
        if (piece == ' ') return false;
        bool isWhite = (piece >= 'A' && piece <= 'Z');
        if (isWhite != whiteToMove) return false;
        char target = board[tr][tc];
        if (target != ' ') {
            bool targetWhite = (target >= 'A' && target <= 'Z');
            if (targetWhite == isWhite) return false;
        }
        char lower = isWhite ? (piece + 32) : piece;
        bool valid = false;
        switch (lower) {
            case 'p': valid = ValidatePawn(fr, fc, tr, tc, isWhite); break;
            case 'n': valid = ValidateKnight(fr, fc, tr, tc); break;
            case 'b': valid = ValidateBishop(fr, fc, tr, tc); break;
            case 'r': valid = ValidateRook(fr, fc, tr, tc); break;
            case 'q': valid = ValidateBishop(fr, fc, tr, tc) || ValidateRook(fr, fc, tr, tc); break;
            case 'k': valid = ValidateKing(fr, fc, tr, tc, isWhite); break;
        }
        if (!valid) return false;

        // Verify move doesn't leave own king in check
        char tempBoard[8][8];
        std::memcpy(tempBoard, board, sizeof(board));
        const_cast<ChessEngine*>(this)->board[tr][tc] = piece;
        const_cast<ChessEngine*>(this)->board[fr][fc] = ' ';
        bool inCheck = IsKingInCheck(isWhite);
        std::memcpy(const_cast<ChessEngine*>(this)->board, tempBoard, sizeof(board));
        return !inCheck;
    }

    void MakeMove(int fr, int fc, int tr, int tc) {
        char piece = board[fr][fc];
        bool isWhite = (piece >= 'A' && piece <= 'Z');

        // En passant capture
        char lower = isWhite ? (piece + 32) : piece;
        if (lower == 'p' && fc != tc && board[tr][tc] == ' ') {
            board[fr][tc] = ' ';
        }

        // Castling
        if (lower == 'k' && (std::abs)(tc - fc) == 2) {
            if (tc == 6) { board[fr][5] = board[fr][7]; board[fr][7] = ' '; }
            if (tc == 2) { board[fr][3] = board[fr][0]; board[fr][0] = ' '; }
        }

        board[tr][tc] = piece;
        board[fr][fc] = ' ';

        // Pawn promotion
        if (lower == 'p' && (tr == 0 || tr == 7)) {
            board[tr][tc] = isWhite ? 'Q' : 'q';
        }

        // Update castling rights
        if (piece == 'K') whiteKingMoved = true;
        if (piece == 'k') blackKingMoved = true;
        if (fr == 7 && fc == 0) whiteRookAMoved = true;
        if (fr == 7 && fc == 7) whiteRookHMoved = true;
        if (fr == 0 && fc == 0) blackRookAMoved = true;
        if (fr == 0 && fc == 7) blackRookHMoved = true;

        // En passant tracking
        enPassantCol = -1;
        if (lower == 'p' && (std::abs)(tr - fr) == 2) enPassantCol = fc;

        whiteToMove = !whiteToMove;

        // Check for checkmate/stalemate
        if (!HasAnyLegalMove(whiteToMove)) {
            gameOver = true;
            if (IsKingInCheck(whiteToMove))
                result = isWhite ? "White wins by checkmate!" : "Black wins by checkmate!";
            else
                result = "Stalemate — draw!";
        }
    }

    bool IsKingInCheck(bool forWhite) const {
        int kr = -1, kc = -1;
        char king = forWhite ? 'K' : 'k';
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (board[r][c] == king) { kr = r; kc = c; }
        if (kr < 0) return true;
        return IsSquareAttacked(kr, kc, !forWhite);
    }

    char board[8][8];
    bool whiteToMove;
    bool gameOver;
    std::string result;

private:
    bool whiteKingMoved, blackKingMoved;
    bool whiteRookAMoved, whiteRookHMoved;
    bool blackRookAMoved, blackRookHMoved;
    int enPassantCol;

    bool InBounds(int r, int c) const { return r >= 0 && r < 8 && c >= 0 && c < 8; }

    bool ValidatePawn(int fr, int fc, int tr, int tc, bool isWhite) const {
        int dir = isWhite ? -1 : 1;
        int startRow = isWhite ? 6 : 1;
        if (fc == tc) {
            if (tr == fr + dir && board[tr][tc] == ' ') return true;
            if (fr == startRow && tr == fr + 2 * dir && board[fr + dir][fc] == ' ' && board[tr][tc] == ' ') return true;
        }
        if ((std::abs)(tc - fc) == 1 && tr == fr + dir) {
            if (board[tr][tc] != ' ') return true;
            int epRow = isWhite ? 3 : 4;
            if (fr == epRow && tc == enPassantCol && board[fr][tc] != ' ') return true;
        }
        return false;
    }

    bool ValidateKnight(int fr, int fc, int tr, int tc) const {
        int dr = (std::abs)(tr - fr), dc = (std::abs)(tc - fc);
        return (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
    }

    bool ValidateBishop(int fr, int fc, int tr, int tc) const {
        int dr = tr - fr, dc = tc - fc;
        if ((std::abs)(dr) != (std::abs)(dc) || dr == 0) return false;
        int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
        for (int r = fr + sr, c = fc + sc; r != tr; r += sr, c += sc)
            if (board[r][c] != ' ') return false;
        return true;
    }

    bool ValidateRook(int fr, int fc, int tr, int tc) const {
        if (fr != tr && fc != tc) return false;
        int sr = (tr > fr) ? 1 : (tr < fr) ? -1 : 0;
        int sc = (tc > fc) ? 1 : (tc < fc) ? -1 : 0;
        for (int r = fr + sr, c = fc + sc; r != tr || c != tc; r += sr, c += sc)
            if (board[r][c] != ' ') return false;
        return true;
    }

    bool ValidateKing(int fr, int fc, int tr, int tc, bool isWhite) const {
        int dr = (std::abs)(tr - fr), dc = (std::abs)(tc - fc);
        if (dr <= 1 && dc <= 1 && (dr + dc > 0)) return true;
        // Castling
        if (dr == 0 && dc == 2 && !IsKingInCheck(isWhite)) {
            bool kingMoved = isWhite ? whiteKingMoved : blackKingMoved;
            if (kingMoved) return false;
            if (tc == 6) {
                bool rookMoved = isWhite ? whiteRookHMoved : blackRookHMoved;
                if (rookMoved) return false;
                if (board[fr][5] != ' ' || board[fr][6] != ' ') return false;
                if (IsSquareAttacked(fr, 5, !isWhite)) return false;
                return true;
            }
            if (tc == 2) {
                bool rookMoved = isWhite ? whiteRookAMoved : blackRookAMoved;
                if (rookMoved) return false;
                if (board[fr][1] != ' ' || board[fr][2] != ' ' || board[fr][3] != ' ') return false;
                if (IsSquareAttacked(fr, 3, !isWhite)) return false;
                return true;
            }
        }
        return false;
    }

    bool IsSquareAttacked(int r, int c, bool byWhite) const {
        for (int ar = 0; ar < 8; ar++) {
            for (int ac = 0; ac < 8; ac++) {
                char p = board[ar][ac];
                if (p == ' ') continue;
                bool pw = (p >= 'A' && p <= 'Z');
                if (pw != byWhite) continue;
                char lp = pw ? (p + 32) : p;
                switch (lp) {
                    case 'p': {
                        int dir = pw ? -1 : 1;
                        if (ar + dir == r && (std::abs)(ac - c) == 1) return true;
                        break;
                    }
                    case 'n': {
                        int dr = (std::abs)(r - ar), dc = (std::abs)(c - ac);
                        if ((dr == 2 && dc == 1) || (dr == 1 && dc == 2)) return true;
                        break;
                    }
                    case 'b': if (ValidateBishop(ar, ac, r, c)) return true; break;
                    case 'r': if (ValidateRook(ar, ac, r, c)) return true; break;
                    case 'q': if (ValidateBishop(ar, ac, r, c) || ValidateRook(ar, ac, r, c)) return true; break;
                    case 'k': {
                        int dr = (std::abs)(r - ar), dc = (std::abs)(c - ac);
                        if (dr <= 1 && dc <= 1 && (dr + dc > 0)) return true;
                        break;
                    }
                }
            }
        }
        return false;
    }

    bool HasAnyLegalMove(bool forWhite) const {
        for (int fr = 0; fr < 8; fr++) {
            for (int fc = 0; fc < 8; fc++) {
                char p = board[fr][fc];
                if (p == ' ') continue;
                bool pw = (p >= 'A' && p <= 'Z');
                if (pw != forWhite) continue;
                for (int tr = 0; tr < 8; tr++)
                    for (int tc = 0; tc < 8; tc++)
                        if (IsLegalMove(fr, fc, tr, tc)) return true;
            }
        }
        return false;
    }
};

} // namespace TalkMe
