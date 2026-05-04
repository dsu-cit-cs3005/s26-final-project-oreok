#include "Arena.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace
{

using RobotSummaryFn = const char* (*)();

std::string trim(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

void bresenham(int r0, int c0, int r1, int c1, std::vector<std::pair<int, int>>& out)
{
    out.clear();
    int x = c0, y = r0, x1 = c1, y1 = r1;
    const int dx = std::abs(x1 - x), sx = x < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y), sy = y < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        out.push_back({y, x});
        if (x == x1 && y == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y += sy;
        }
    }
}

void railgun_cells(int sr, int sc, int tr, int tc, int H, int W, std::vector<std::pair<int, int>>& out)
{
    out.clear();
    if (sr == tr && sc == tc)
        return;
    std::vector<std::pair<int, int>> seg;
    bresenham(sr, sc, tr, tc, seg);
    for (const auto& p : seg)
    {
        if (p.first == sr && p.second == sc)
            continue;
        out.push_back(p);
    }
    if (out.empty())
        return;
    for (;;)
    {
        const auto prev = out.size() >= 2 ? out[out.size() - 2] : std::pair<int, int>{sr, sc};
        const auto& last = out.back();
        const int nr = last.first + (last.first - prev.first);
        const int nc = last.second + (last.second - prev.second);
        if (nr < 0 || nr >= H || nc < 0 || nc >= W)
            break;
        out.push_back({nr, nc});
    }
}

void wide_ray(int r0, int c0, int dir, int max_steps, int H, int W, std::vector<std::pair<int, int>>& cells)
{
    if (dir < 1 || dir > 8)
        return;
    const int dr = directions[dir].first;
    const int dc = directions[dir].second;
    const int pa_r = -dc, pa_c = dr;
    const int pb_r = dc, pb_c = -dr;
    for (int d = 1; d <= max_steps; ++d)
    {
        const int br = r0 + dr * d;
        const int bc = c0 + dc * d;
        const int offs[3][2] = {{0, 0}, {pa_r, pa_c}, {pb_r, pb_c}};
        for (const auto& off : offs)
        {
            const int rr = br + off[0];
            const int cc = bc + off[1];
            if (rr >= 0 && rr < H && cc >= 0 && cc < W)
                cells.push_back({rr, cc});
        }
    }
}

int sign1(int v)
{
    return (v > 0) - (v < 0);
}

void flame_cells(int rr, int cc, int tr, int tc, int H, int W, std::vector<std::pair<int, int>>& out)
{
    out.clear();
    const int dr = sign1(tr - rr);
    const int dc = sign1(tc - cc);
    if (dr == 0 && dc == 0)
        return;
    int dir = 0;
    for (int d = 1; d <= 8; ++d)
        if (directions[d].first == dr && directions[d].second == dc)
            dir = d;
    if (!dir)
        return;
    wide_ray(rr, cc, dir, 4, H, W, out);
}

} // namespace

int Arena::roll_damage(std::mt19937& rng, WeaponType w)
{
    static const struct
    {
        WeaponType w;
        int lo;
        int hi;
    } tbl[] = {{railgun, 10, 20}, {hammer, 50, 60}, {grenade, 10, 40}, {flamethrower, 30, 50}};
    for (const auto& e : tbl)
        if (e.w == w)
            return std::uniform_int_distribution<int>(e.lo, e.hi)(rng);
    return 1;
}

Arena::Arena(std::string config_path) : config_path_(std::move(config_path))
{
    rng_.seed(std::random_device{}());
}

Arena::~Arena()
{
    unload_robots();
}

void Arena::unload_robots()
{
    for (auto& lr : robots_)
    {
        delete lr.robot;
        lr.robot = nullptr;
        if (lr.dl_handle)
            dlclose(lr.dl_handle);
    }
    robots_.clear();
}

bool Arena::load_config()
{
    std::ifstream in(config_path_);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        for (char& c : line)
            if (c == ':')
                c = ' ';
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "Arena_Size")
            ss >> cfg_.height >> cfg_.width;
        else if (key == "Max_Rounds")
            ss >> cfg_.max_rounds;
        else if (key == "Sleep_interval")
            ss >> cfg_.sleep_interval;
        else if (key == "Game_State_Live")
        {
            std::string v;
            ss >> v;
            cfg_.game_state_live = (v == "true" || v == "True" || v == "1");
        }
        else if (key == "Flamethrowers")
            ss >> cfg_.flamethrowers;
        else if (key == "Pits")
            ss >> cfg_.pits;
        else if (key == "Mounds")
            ss >> cfg_.mounds;
    }
    if (cfg_.height <= 0 || cfg_.width <= 0)
        return false;
    height_ = cfg_.height;
    width_ = cfg_.width;
    return true;
}

bool Arena::in_bounds(int r, int c) const
{
    return r >= 0 && r < height_ && c >= 0 && c < width_;
}

bool Arena::init_board_and_obstacles()
{
    terrain_.assign(static_cast<size_t>(height_), std::vector<char>(static_cast<size_t>(width_), '\0'));
    occupant_.assign(static_cast<size_t>(height_), std::vector<int>(static_cast<size_t>(width_), -1));
    std::uniform_int_distribution<int> R(0, height_ - 1), C(0, width_ - 1);

    const std::pair<char, int> packs[] = {{'M', cfg_.mounds}, {'P', cfg_.pits}, {'F', cfg_.flamethrowers}};
    for (auto [sym, need] : packs)
    {
        for (int got = 0; got < need;)
        {
            const int r = R(rng_), c = C(rng_);
            if (terrain_[static_cast<size_t>(r)][static_cast<size_t>(c)] != '\0')
                continue;
            terrain_[static_cast<size_t>(r)][static_cast<size_t>(c)] = sym;
            ++got;
        }
    }
    return true;
}

bool Arena::compile_and_load_robots()
{
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator("."))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string fname = entry.path().filename().string();
        if (fname.size() < 11 || fname.compare(0, 6, "Robot_") != 0)
            continue;
        if (fname.compare(fname.size() - 4, 4, ".cpp") != 0)
            continue;

        const std::string cpp = entry.path().string();
        const std::string base = fname.substr(0, fname.size() - 4);
        const std::string so = "lib" + base + ".so";
        const std::string load_path = "./" + so;

        if (std::system(("g++ -shared -fPIC -o " + so + " " + cpp + " RobotBase.o -I. -std=c++20").c_str()) != 0)
            return false;
        void* h = dlopen(load_path.c_str(), RTLD_LAZY);
        if (!h)
            return false;
        auto* make = reinterpret_cast<RobotFactory>(dlsym(h, "create_robot"));
        auto* summ = reinterpret_cast<RobotSummaryFn>(dlsym(h, "robot_summary"));
        if (!make || !summ)
        {
            dlclose(h);
            return false;
        }
        RobotBase* rb = make();
        if (!rb)
        {
            dlclose(h);
            return false;
        }
        robots_.push_back({rb, h, true});
    }
    return !robots_.empty();
}

void Arena::occ_clear(int idx)
{
    int r = 0, c = 0;
    robots_[static_cast<size_t>(idx)].robot->get_current_location(r, c);
    if (in_bounds(r, c))
        occupant_[static_cast<size_t>(r)][static_cast<size_t>(c)] = -1;
}

void Arena::occ_place(int idx, int r, int c)
{
    robots_[static_cast<size_t>(idx)].robot->move_to(r, c);
    occupant_[static_cast<size_t>(r)][static_cast<size_t>(c)] = idx;
}

bool Arena::init_robot_positions()
{
    static const char sym[] = "@#$%&!+*^~=:?";
    std::uniform_int_distribution<int> R(0, height_ - 1), C(0, width_ - 1);

    for (size_t i = 0; i < robots_.size(); ++i)
    {
        for (;;)
        {
            const int r = R(rng_), c = C(rng_);
            if (terrain_[static_cast<size_t>(r)][static_cast<size_t>(c)] != '\0')
                continue;
            if (occupant_[static_cast<size_t>(r)][static_cast<size_t>(c)] != -1)
                continue;
            robots_[i].robot->set_boundaries(height_, width_);
            robots_[i].robot->m_character = sym[i % (sizeof sym - 1)];
            occ_place(static_cast<int>(i), r, c);
            break;
        }
    }
    return true;
}

std::vector<RadarObj> Arena::scan_radar(int self_idx, int radar_direction) const
{
    std::vector<RadarObj> results;
    int r0 = 0, c0 = 0;
    robots_[static_cast<size_t>(self_idx)].robot->get_current_location(r0, c0);

    auto push_vis = [&](int r, int c) {
        if (!in_bounds(r, c) || (r == r0 && c == c0))
            return;
        const int o = occupant_[static_cast<size_t>(r)][static_cast<size_t>(c)];
        if (o >= 0)
        {
            const bool dead = !robots_[static_cast<size_t>(o)].alive;
            results.emplace_back(dead ? 'X' : 'R', r, c);
            return;
        }
        const char t = terrain_[static_cast<size_t>(r)][static_cast<size_t>(c)];
        if (t)
            results.emplace_back(t, r, c);
    };

    if (radar_direction == 0)
    {
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
                if (dr || dc)
                    push_vis(r0 + dr, c0 + dc);
        return results;
    }

    std::vector<std::pair<int, int>> cells;
    wide_ray(r0, c0, radar_direction, std::max(height_, width_), height_, width_, cells);
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    for (const auto& p : cells)
        push_vis(p.first, p.second);
    return results;
}

void Arena::damage_robot(int target_idx, WeaponType weapon)
{
    RobotBase* t = robots_[static_cast<size_t>(target_idx)].robot;
    const int raw = roll_damage(rng_, weapon);
    const int arm = t->get_armor();
    const int dmg = std::max(
        1, static_cast<int>(std::lround(static_cast<double>(raw) * (1.0 - 0.1 * static_cast<double>(arm)))));
    t->take_damage(dmg);
    t->reduce_armor(1);
}

bool Arena::handle_shot(int shooter_idx, int shot_row, int shot_col)
{
    if (!in_bounds(shot_row, shot_col))
        return false;

    RobotBase* shooter = robots_[static_cast<size_t>(shooter_idx)].robot;
    const WeaponType w = shooter->get_weapon();
    int sr = 0, sc = 0;
    shooter->get_current_location(sr, sc);

    std::vector<std::pair<int, int>> cells;

    if (w == railgun)
        railgun_cells(sr, sc, shot_row, shot_col, height_, width_, cells);
    else if (w == flamethrower)
        flame_cells(sr, sc, shot_row, shot_col, height_, width_, cells);
    else if (w == grenade)
    {
        if (shooter->get_grenades() <= 0)
            return false;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
            {
                const int rr = shot_row + dr, cc = shot_col + dc;
                if (in_bounds(rr, cc))
                    cells.push_back({rr, cc});
            }
        shooter->decrement_grenades();
    }
    else if (w == hammer)
    {
        const int cheb = std::max(std::abs(shot_row - sr), std::abs(shot_col - sc));
        if (cheb > 1 || cheb == 0)
            return false;
        cells.push_back({shot_row, shot_col});
    }

    std::unordered_set<int> hit;
    for (const auto& p : cells)
    {
        const int o = occupant_[static_cast<size_t>(p.first)][static_cast<size_t>(p.second)];
        if (o < 0 || o == shooter_idx || !robots_[static_cast<size_t>(o)].alive)
            continue;
        hit.insert(o);
    }
    for (int t : hit)
    {
        damage_robot(t, w);
        if (robots_[static_cast<size_t>(t)].robot->get_health() <= 0)
            robots_[static_cast<size_t>(t)].alive = false;
    }
    return true;
}

void Arena::handle_move(int mover_idx, int direction, int distance)
{
    RobotBase* bot = robots_[static_cast<size_t>(mover_idx)].robot;
    auto& lr = robots_[static_cast<size_t>(mover_idx)];
    if (!lr.alive || bot->get_move_speed() <= 0 || direction < 1 || direction > 8)
        return;

    const int steps = std::min(distance, bot->get_move_speed());
    const int dr = directions[direction].first;
    const int dc = directions[direction].second;
    int cur_r = 0, cur_c = 0;
    bot->get_current_location(cur_r, cur_c);

    for (int s = 0; s < steps; ++s)
    {
        const int nr = cur_r + dr, nc = cur_c + dc;
        if (!in_bounds(nr, nc))
            break;
        const char tile = terrain_[static_cast<size_t>(nr)][static_cast<size_t>(nc)];
        if (occupant_[static_cast<size_t>(nr)][static_cast<size_t>(nc)] != -1 || tile == 'M')
            break;

        occ_clear(mover_idx);

        if (tile == 'P')
        {
            bot->move_to(nr, nc);
            bot->disable_movement();
            occupant_[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = mover_idx;
            break;
        }
        if (tile == 'F')
        {
            damage_robot(mover_idx, flamethrower);
            bot->move_to(nr, nc);
            occupant_[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = mover_idx;
            cur_r = nr;
            cur_c = nc;
            if (bot->get_health() <= 0)
            {
                lr.alive = false;
                terrain_[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = '\0';
            }
            continue;
        }
        bot->move_to(nr, nc);
        occupant_[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = mover_idx;
        cur_r = nr;
        cur_c = nc;
    }
}

void Arena::print_state() const
{
    std::cout << "\n=========== Round " << round_number_ << " ===========\n    ";
    for (int c = 0; c < width_; ++c)
        std::cout << (c % 10);
    std::cout << '\n';
    for (int r = 0; r < height_; ++r)
    {
        std::cout.width(3);
        std::cout << r << ' ';
        std::cout.width(0);
        for (int c = 0; c < width_; ++c)
        {
            const int o = occupant_[static_cast<size_t>(r)][static_cast<size_t>(c)];
            if (o >= 0)
            {
                const auto& L = robots_[static_cast<size_t>(o)];
                std::cout << (L.alive ? L.robot->m_character : static_cast<char>('X'));
            }
            else
            {
                const char t = terrain_[static_cast<size_t>(r)][static_cast<size_t>(c)];
                std::cout << (t ? t : '.');
            }
        }
        std::cout << '\n';
    }
    for (size_t i = 0; i < robots_.size(); ++i)
    {
        std::cout << robots_[i].robot->print_stats();
        if (!robots_[i].alive)
            std::cout << " [eliminated]";
        std::cout << '\n';
    }
}

int Arena::living_count() const
{
    int n = 0;
    for (const auto& lr : robots_)
        if (lr.alive && lr.robot->get_health() > 0)
            ++n;
    return n;
}

int Arena::sole_winner() const
{
    int idx = -1, cnt = 0;
    for (int i = 0; i < static_cast<int>(robots_.size()); ++i)
        if (robots_[static_cast<size_t>(i)].alive && robots_[static_cast<size_t>(i)].robot->get_health() > 0)
        {
            idx = i;
            ++cnt;
        }
    return cnt == 1 ? idx : -1;
}

int Arena::run()
{
    if (!load_config() || !init_board_and_obstacles() || !compile_and_load_robots() || !init_robot_positions())
        return 1;

    round_number_ = 1;
    while (round_number_ <= cfg_.max_rounds)
    {
        for (size_t i = 0; i < robots_.size(); ++i)
        {
            print_state();

            const int win = sole_winner();
            if (win >= 0)
            {
                std::cout << "\nWinner: robot index " << win << " (" << robots_[static_cast<size_t>(win)].robot->m_name
                          << ")\n";
                return 0;
            }
            if (living_count() == 0)
                return 0;

            auto& L = robots_[i];
            if (!L.alive || L.robot->get_health() <= 0)
            {
                if (++round_number_ > cfg_.max_rounds)
                    return 0;
                continue;
            }

            int rdir = 0;
            L.robot->get_radar_direction(rdir);
            L.robot->process_radar_results(scan_radar(static_cast<int>(i), rdir));

            int tr = 0, tc = 0;
            if (L.robot->get_shot_location(tr, tc))
                handle_shot(static_cast<int>(i), tr, tc);
            else
            {
                int md = 0, dist = 0;
                L.robot->get_move_direction(md, dist);
                handle_move(static_cast<int>(i), md, dist);
            }

            if (cfg_.game_state_live && cfg_.sleep_interval > 0.0)
                std::this_thread::sleep_for(std::chrono::duration<double>(cfg_.sleep_interval));

            if (++round_number_ > cfg_.max_rounds)
                return 0;

            const int w2 = sole_winner();
            if (w2 >= 0)
            {
                print_state();
                std::cout << "\nWinner: robot index " << w2 << " (" << robots_[static_cast<size_t>(w2)].robot->m_name
                          << ")\n";
                return 0;
            }
        }
    }
    return 0;
}
