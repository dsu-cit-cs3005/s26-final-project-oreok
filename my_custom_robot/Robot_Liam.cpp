#include "RobotBase.h"
#include <set>
#include <utility>
#include <vector>

class Robot_Liam : public RobotBase
{
private:
    int scan_dir_ = 1;
    bool target_found_ = false;
    int target_row_ = -1;
    int target_col_ = -1;
    std::set<std::pair<int,int>> obstacles_;

    void update_obstacles(const std::vector<RadarObj>& results)
    {
        for (const auto& obj : results)
            if (obj.m_type == 'M' || obj.m_type == 'P' || obj.m_type == 'F')
                obstacles_.insert({obj.m_row, obj.m_col});
    }

    bool is_clear(int r, int c) const
    {
        return r >= 0 && r < m_board_row_max
            && c >= 0 && c < m_board_col_max
            && obstacles_.count({r, c}) == 0;
    }

public:
    Robot_Liam() : RobotBase(4, 3, railgun)
    {
        m_name = "Liam";
    }

    void get_radar_direction(int& dir) override
    {
        dir = scan_dir_;
    }

    void process_radar_results(const std::vector<RadarObj>& results) override
    {
        update_obstacles(results);
        target_found_ = false;

        for (const auto& obj : results)
        {
            if (obj.m_type == 'R')
            {
                target_row_ = obj.m_row;
                target_col_ = obj.m_col;
                target_found_ = true;
                break;
            }
        }

        // Lane cleared or never had target — sweep to the next direction
        if (!target_found_)
            scan_dir_ = (scan_dir_ % 8) + 1;
    }

    bool get_shot_location(int& row, int& col) override
    {
        if (!target_found_)
            return false;
        row = target_row_;
        col = target_col_;
        return true;
    }

    void get_move_direction(int& dir, int& dist) override
    {
        int r = 0, c = 0;
        get_current_location(r, c);

        // Advance in the scan direction to sweep fresh lanes
        const int nr = r + directions[scan_dir_].first;
        const int nc = c + directions[scan_dir_].second;
        if (is_clear(nr, nc))
        {
            // Walk the full path and stop before any known obstacle
            int safe = 0;
            for (int step = 1; step <= get_move_speed(); ++step)
            {
                const int sr = r + directions[scan_dir_].first  * step;
                const int sc = c + directions[scan_dir_].second * step;
                if (!is_clear(sr, sc))
                    break;
                safe = step;
            }
            dir = scan_dir_;
            dist = safe > 0 ? safe : 1;
            return;
        }

        // Blocked in scan direction — try all other directions to unstick
        for (int d = 1; d <= 8; ++d)
        {
            if (d == scan_dir_)
                continue;
            const int tr = r + directions[d].first;
            const int tc = c + directions[d].second;
            if (is_clear(tr, tc))
            {
                dir = d;
                dist = 1;
                return;
            }
        }

        dir = 1;
        dist = 0;
    }
};

extern "C" RobotBase* create_robot()
{
    return new Robot_Liam();
}

extern "C" const char* robot_summary()
{
    return "Sweeps lanes, railguns through all in the line.";
}
