#pragma once
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <algorithm>

namespace TalkMe {

class FlappyBird {
public:
    static constexpr float kGravity = 800.0f;
    static constexpr float kJumpVelocity = -280.0f;
    static constexpr float kPipeSpeed = 150.0f;
    static constexpr float kPipeGap = 140.0f;
    static constexpr float kPipeWidth = 50.0f;
    static constexpr float kBirdSize = 20.0f;
    static constexpr float kWorldW = 400.0f;
    static constexpr float kWorldH = 500.0f;

    struct Pipe {
        float x;
        float gapY;
        bool scored = false;
    };

    bool active = false;
    bool gameOver = false;
    int score = 0;
    int bestScore = 0;
    float birdY = 250.0f;
    float birdVel = 0.0f;
    std::vector<Pipe> pipes;
    float spawnTimer = 0.0f;
    std::string playerName;

    // Leaderboard
    struct LeaderboardEntry { std::string name; int score; };
    std::vector<LeaderboardEntry> localLeaderboard;

    void Reset(const std::string& name) {
        playerName = name;
        active = true;
        gameOver = false;
        score = 0;
        birdY = kWorldH * 0.5f;
        birdVel = 0;
        pipes.clear();
        spawnTimer = 0;
        srand((unsigned)time(nullptr));
    }

    void Jump() {
        if (!active) return;
        if (gameOver) { Reset(playerName); return; }
        birdVel = kJumpVelocity;
    }

    void Update(float dt) {
        if (!active || gameOver) return;

        birdVel += kGravity * dt;
        birdY += birdVel * dt;

        // Ground/ceiling
        if (birdY < 0) { birdY = 0; birdVel = 0; }
        if (birdY > kWorldH - kBirdSize) { Die(); return; }

        // Spawn pipes
        spawnTimer += dt;
        if (spawnTimer > 1.8f) {
            Pipe p;
            p.x = kWorldW + kPipeWidth;
            p.gapY = 80.0f + (float)(rand() % (int)(kWorldH - 200.0f));
            pipes.push_back(p);
            spawnTimer = 0;
        }

        // Move pipes and check collision
        float birdX = 60.0f;
        for (auto& p : pipes) {
            p.x -= kPipeSpeed * dt;

            // Score
            if (!p.scored && p.x + kPipeWidth < birdX) {
                score++;
                p.scored = true;
            }

            // Collision
            if (birdX + kBirdSize > p.x && birdX < p.x + kPipeWidth) {
                if (birdY < p.gapY - kPipeGap * 0.5f || birdY + kBirdSize > p.gapY + kPipeGap * 0.5f) {
                    Die();
                    return;
                }
            }
        }

        // Remove off-screen pipes
        pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
            [](const Pipe& p) { return p.x + kPipeWidth < -10; }), pipes.end());
    }

private:
    void Die() {
        gameOver = true;
        if (score > bestScore) bestScore = score;
        // Add to leaderboard
        bool found = false;
        for (auto& e : localLeaderboard) {
            if (e.name == playerName) {
                if (score > e.score) e.score = score;
                found = true;
                break;
            }
        }
        if (!found) localLeaderboard.push_back({ playerName, score });
        std::sort(localLeaderboard.begin(), localLeaderboard.end(),
            [](const LeaderboardEntry& a, const LeaderboardEntry& b) { return a.score > b.score; });
        if (localLeaderboard.size() > 10) localLeaderboard.resize(10);
    }
};

} // namespace TalkMe
