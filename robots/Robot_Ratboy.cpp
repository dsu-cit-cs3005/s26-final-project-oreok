#include "RobotBase.h"
#include <vector>
#include <iostream>
#include <algorithm> // For std::find_if

class Robot_Ratboy : public RobotBase 
{
private:
    bool m_moving_down = true; // Tracks vertical movement direction
    int to_shoot_row = -1;   // Tracks the row of the next target to shoot
    int to_shoot_col = -1;   // Tracks the column of the next target to shoot
    
    std::vector<RadarObj> known_obstacles; // Permanent obstacle list

    // Helper function to determine if a cell is an obstacle
    bool is_obstacle(int row, int col) const 
    {
        return std::any_of(known_obstacles.begin(), known_obstacles.end(), 
                           [&](const RadarObj& obj) {
                               return obj.m_row == row && obj.m_col == col;
                           });
    }

    // Clears the target when no enemy is found
    void clear_target() 
    {
        to_shoot_row = -1;
        to_shoot_col = -1;
    }

    // Helper function to add an obstacle to the list if it's not already there
    void add_obstacle(const RadarObj& obj) 
    {
        if ((obj.m_type == 'M' || obj.m_type == 'P' || obj.m_type == 'F') && 
            !is_obstacle(obj.m_row, obj.m_col)) 
        {
            known_obstacles.push_back(obj);
        }
    }

public:
    Robot_Ratboy() : RobotBase(3, 4, railgun) {} // Initialize with 3 movement, 4 armor, railgun

    // Radar location for scanning in one of the 8 directions
    virtual void get_radar_direction(int& radar_direction) override 
    {
        int current_row, current_col;
        get_current_location(current_row, current_col);

        // Decide radar direction
        radar_direction = (current_col > 0) ? 7 : 3; // Left or Right
    }

    // Processes radar results and updates known obstacles and target
    virtual void process_radar_results(const std::vector<RadarObj>& radar_results) override 
    {
        clear_target();

        for (const auto& obj : radar_results) 
        {
            // Add static obstacles to the obstacle list
            add_obstacle(obj);

            // Identify the first enemy found as the target
            if (obj.m_type == 'R' && to_shoot_row == -1 && to_shoot_col == -1) 
            {
                to_shoot_row = obj.m_row;
                to_shoot_col = obj.m_col;
            }
        }
    }

    // Determines the next shot location
    virtual bool get_shot_location(int& shot_row, int& shot_col) override 
    {
        if (to_shoot_row != -1 && to_shoot_col != -1) 
        {
            shot_row = to_shoot_row;
            shot_col = to_shoot_col;
            clear_target(); // Clear target after shooting
            return true;
        }
        return false;
    }

    // Determines the next movement direction
void get_move_direction(int& move_direction, int& move_distance) override 
{
    int current_row, current_col;
    get_current_location(current_row, current_col);
    int move = get_move_speed(); // Max movement range for this robot

    // Step 1: Move left until column == 0
    if (current_col > 0) 
    {
        move_direction = 7; // Left
        move_distance = std::min(move, current_col); // Clamp to avoid going out of bounds
        return;
    }

    // Step 2: Vertical movement once column == 0
    if (m_moving_down) 
    {
        // Move down if not at the bottom
        if (current_row + move < m_board_row_max) 
        {
            move_direction = 5; // Down
            move_distance = std::min(move, m_board_row_max - current_row - 1);
        } 
        else 
        {
            // Switch to moving up
            m_moving_down = false;
            move_direction = 1; // Up
            move_distance = 1;  // Take a single step up
        }
    } 
    else 
    {
        // Move up if not at the top
        if (current_row - move >= 0) 
        {
            move_direction = 1; // Up
            move_distance = std::min(move, current_row);
        } 
        else 
        {
            // Switch to moving down
            m_moving_down = true;
            move_direction = 5; // Down
            move_distance = 1;  // Take a single step down
        }
    }
}

};

// Factory function to create Robot_Ratboy
extern "C" RobotBase* create_robot() 
{
    return new Robot_Ratboy();
}

// Required by grading arena/test harness: keep <= 50 chars.
extern "C" const char* robot_summary()
{
    return "Hugs left wall, railguns nearest target.";
}
