#pragma once
#include <string>

namespace TalkMe {

class TicTacToe {
public:
    bool active = false;
    std::string opponent;
    bool myTurn = false;
    bool isX = true;
    char board[9] = { ' ',' ',' ',' ',' ',' ',' ',' ',' ' };
    std::string result;

    void Reset(const std::string& opp, bool playAsX) {
        active = true;
        opponent = opp;
        isX = playAsX;
        myTurn = playAsX;
        result.clear();
        for (auto& c : board) c = ' ';
    }

    bool MakeMove(int cell, char piece) {
        if (cell < 0 || cell > 8 || board[cell] != ' ') return false;
        board[cell] = piece;
        CheckWin();
        return true;
    }

    int MyPiece() const { return isX ? 'X' : 'O'; }
    int TheirPiece() const { return isX ? 'O' : 'X'; }

private:
    void CheckWin() {
        const int wins[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
        for (auto& w : wins) {
            if (board[w[0]] != ' ' && board[w[0]] == board[w[1]] && board[w[1]] == board[w[2]]) {
                result = std::string(1, board[w[0]]) + " wins!";
                return;
            }
        }
        bool full = true;
        for (auto c : board) if (c == ' ') { full = false; break; }
        if (full) result = "Draw!";
    }
};

} // namespace TalkMe
