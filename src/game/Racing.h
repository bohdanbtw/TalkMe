#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace TalkMe {

struct RaceCar {
    float x = 200.0f, y = 500.0f;
    float angle = -1.5708f;
    float speed = 0.0f;
    float lapProgress = 0.0f;
    int lap = 0;
    int checkpoint = 0;
    std::string name;
};

class RacingGame {
public:
    static constexpr int kTotalLaps = 3;
    static constexpr float kMaxSpeed = 5.0f;
    static constexpr float kAcceleration = 0.08f;
    static constexpr float kBrakeForce = 0.12f;
    static constexpr float kFriction = 0.02f;
    static constexpr float kTurnSpeed = 0.04f;
    static constexpr float kTrackCX = 300.0f;
    static constexpr float kTrackCY = 250.0f;
    static constexpr float kTrackRX = 220.0f;
    static constexpr float kTrackRY = 160.0f;
    static constexpr float kTrackWidth = 60.0f;

    bool active = false;
    bool finished = false;
    std::string winner;
    RaceCar player;
    RaceCar opponent;
    float raceTime = 0.0f;

    void Reset(const std::string& playerName, const std::string& opponentName) {
        player = {};
        player.name = playerName;
        player.x = kTrackCX + kTrackRX;
        player.y = kTrackCY + 10.0f;
        player.angle = -1.5708f;

        opponent = {};
        opponent.name = opponentName;
        opponent.x = kTrackCX + kTrackRX;
        opponent.y = kTrackCY - 10.0f;
        opponent.angle = -1.5708f;

        active = true;
        finished = false;
        winner.clear();
        raceTime = 0.0f;
    }

    void UpdatePlayer(bool accel, bool brake, bool left, bool right, float dt) {
        if (!active || finished) return;
        raceTime += dt;

        if (accel) player.speed = std::min(player.speed + kAcceleration, kMaxSpeed);
        if (brake) player.speed = std::max(player.speed - kBrakeForce, -kMaxSpeed * 0.3f);
        if (!accel && !brake) {
            if (player.speed > 0) player.speed = std::max(0.0f, player.speed - kFriction);
            else player.speed = std::min(0.0f, player.speed + kFriction);
        }
        if (left) player.angle -= kTurnSpeed * (std::abs(player.speed) / kMaxSpeed);
        if (right) player.angle += kTurnSpeed * (std::abs(player.speed) / kMaxSpeed);

        player.x += std::cos(player.angle) * player.speed;
        player.y += std::sin(player.angle) * player.speed;

        // Track boundary (elliptical)
        float dx = player.x - kTrackCX;
        float dy = player.y - kTrackCY;
        float dist = std::sqrt((dx * dx) / (kTrackRX * kTrackRX) + (dy * dy) / (kTrackRY * kTrackRY));
        if (dist > 1.3f || dist < 0.6f) player.speed *= 0.5f;

        UpdateLap(player);
        CheckFinish();
    }

    void UpdateOpponent(float x, float y, float angle, float speed, int lap, int checkpoint) {
        opponent.x = x;
        opponent.y = y;
        opponent.angle = angle;
        opponent.speed = speed;
        opponent.lap = lap;
        opponent.checkpoint = checkpoint;
        CheckFinish();
    }

    bool IsOnTrack(float x, float y) const {
        float dx = x - kTrackCX;
        float dy = y - kTrackCY;
        float dist = std::sqrt((dx * dx) / (kTrackRX * kTrackRX) + (dy * dy) / (kTrackRY * kTrackRY));
        return dist >= 0.6f && dist <= 1.3f;
    }

private:
    void UpdateLap(RaceCar& car) {
        float angle = std::atan2(car.y - kTrackCY, car.x - kTrackCX);
        int newCP = (int)((angle + 3.14159f) / (3.14159f * 2.0f / 4.0f)) % 4;
        if (newCP == (car.checkpoint + 1) % 4) {
            car.checkpoint = newCP;
            if (newCP == 0) car.lap++;
        }
    }

    void CheckFinish() {
        if (finished) return;
        if (player.lap >= kTotalLaps) { finished = true; winner = player.name; }
        else if (opponent.lap >= kTotalLaps) { finished = true; winner = opponent.name; }
    }
};

} // namespace TalkMe
