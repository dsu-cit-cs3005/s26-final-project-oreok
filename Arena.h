#pragma once

#include <random>
#include <string>
#include <utility>
#include <vector>

#include "RobotBase.h"

struct ArenaConfig
{
    int height = 0;
    int width = 0;
    int max_rounds = 10000;
    double sleep_interval = 0.0;
    bool game_state_live = true;
    int flamethrowers = 0;
    int pits = 0;
    int mounds = 0;
};

class Arena
{
public:
    explicit Arena(std::string config_path);
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    int run();

private:
    std::string config_path_;
    ArenaConfig cfg_{};
    int height_ = 0;
    int width_ = 0;

    std::vector<std::vector<char>> terrain_;
    struct LoadedRobot
    {
        RobotBase* robot = nullptr;
        void* dl_handle = nullptr;
        bool alive = true;
    };
    std::vector<LoadedRobot> robots_;
    std::vector<std::vector<int>> occupant_;

    int round_number_ = 0;
    std::mt19937 rng_{};

    bool load_config();
    bool init_board_and_obstacles();
    bool init_robot_positions();
    bool compile_and_load_robots();
    void unload_robots();

    bool in_bounds(int r, int c) const;
    void occ_clear(int idx);
    void occ_place(int idx, int r, int c);

    std::vector<RadarObj> scan_radar(int self_idx, int dir) const;
    bool handle_shot(int shooter_idx, int tr, int tc);
    void damage_robot(int target_idx, WeaponType w);
    void handle_move(int idx, int direction, int distance);
    void print_state() const;

    int living_count() const;
    int sole_winner() const; // -1 if not exactly one survivor

    static int roll_damage(std::mt19937& rng, WeaponType w);
};
