#include "map.h"
#include "lightmap.h"
#include "output.h"
#include "rng.h"
#include "game.h"
#include "line.h"
#include "options.h"
#include "item_factory.h"
#include "mapbuffer.h"
#include "translations.h"
#include "monstergenerator.h"
#include "sounds.h"
#include "debug.h"
#include "messages.h"
#include "mapsharing.h"

#include <cmath>
#include <stdlib.h>
#include <fstream>

extern bool is_valid_in_w_terrain(int,int);

#include "overmapbuffer.h"

#define SGN(a) (((a)<0) ? -1 : 1)
#define INBOUNDS(x, y) \
 (x >= 0 && x < SEEX * my_MAPSIZE && y >= 0 && y < SEEY * my_MAPSIZE)
#define dbg(x) DebugLog((DebugLevel)(x),D_MAP) << __FILE__ << ":" << __LINE__ << ": "

enum astar_list {
 ASL_NONE,
 ASL_OPEN,
 ASL_CLOSED
};

// Map stack methods.
size_t map_stack::size() const
{
    return mystack->size();
}

bool map_stack::empty() const
{
    return mystack->empty();
}

std::list<item>::iterator map_stack::erase( std::list<item>::iterator it )
{
    return myorigin->i_rem(location, it);
}

void map_stack::push_back( const item &newitem )
{
    myorigin->add_item_or_charges(location.x, location.y, newitem);
}

void map_stack::insert_at( std::list<item>::iterator index,
                           const item &newitem )
{
    myorigin->add_item_at( location, index, newitem );
}

std::list<item>::iterator map_stack::begin()
{
    return mystack->begin();
}

std::list<item>::iterator map_stack::end()
{
    return mystack->end();
}

std::list<item>::const_iterator map_stack::begin() const
{
    return mystack->cbegin();
}

std::list<item>::const_iterator map_stack::end() const
{
    return mystack->cend();
}

std::list<item>::reverse_iterator map_stack::rbegin()
{
    return mystack->rbegin();
}

std::list<item>::reverse_iterator map_stack::rend()
{
    return mystack->rend();
}

std::list<item>::const_reverse_iterator map_stack::rbegin() const
{
    return mystack->crbegin();
}

std::list<item>::const_reverse_iterator map_stack::rend() const
{
    return mystack->crend();
}

item &map_stack::front()
{
    return mystack->front();
}

item &map_stack::operator[]( size_t index )
{
    return *(std::next(mystack->begin(), index));
}

// Map class methods.

map::map(int mapsize)
{
    nulter = t_null;
    my_MAPSIZE = mapsize;
#ifdef ZLEVELS
    grid.resize( my_MAPSIZE * my_MAPSIZE * OVERMAP_LAYERS, nullptr );
#else
    grid.resize( my_MAPSIZE * my_MAPSIZE, nullptr );
#endif
    dbg(D_INFO) << "map::map(): my_MAPSIZE: " << my_MAPSIZE;
    veh_in_active_range = true;
    transparency_cache_dirty = true;
    outside_cache_dirty = true;
    memset(veh_exists_at, 0, sizeof(veh_exists_at));
    traplocs.resize( traplist.size() );
}

map::~map()
{
}

// Vehicle functions

VehicleList map::get_vehicles(){
   return get_vehicles(0,0,SEEX*my_MAPSIZE, SEEY*my_MAPSIZE);
}

void map::reset_vehicle_cache()
{
    clear_vehicle_cache();
    // Cache all vehicles
    veh_in_active_range = false;
    for( const auto & elem : vehicle_list ) {
        update_vehicle_cache( elem, true );
    }
}

void map::update_vehicle_cache( vehicle *veh, const bool brand_new )
{
    veh_in_active_range = true;
    if( !brand_new ) {
        // Existing must be cleared
        auto it = veh_cached_parts.begin();
        const auto end = veh_cached_parts.end();
        while( it != end ) {
            if( it->second.first == veh ) {
                const auto &p = it->first;
                if( inbounds( p.x, p.y ) ) {
                    veh_exists_at[p.x][p.y] = false;
                }
                veh_cached_parts.erase( it++ );
            } else {
                ++it;
            }
        }
    }
    // Get parts
    std::vector<vehicle_part> &parts = veh->parts;
    const point gpos = veh->global_pos();
    int partid = 0;
    for( std::vector<vehicle_part>::iterator it = parts.begin(),
         end = parts.end(); it != end; ++it, ++partid ) {
        if( it->removed ) {
            continue;
        }
        const point p = gpos + it->precalc[0];
        veh_cached_parts.insert( std::make_pair( p,
                                 std::make_pair( veh, partid ) ) );
        if( inbounds( p.x, p.y ) ) {
            veh_exists_at[p.x][p.y] = true;
        }
    }
}

void map::clear_vehicle_cache()
{
    while( veh_cached_parts.size() ) {
        const auto part = veh_cached_parts.begin();
        const auto &p = part->first;
        if( inbounds( p.x, p.y ) ) {
            veh_exists_at[p.x][p.y] = false;
        }
        veh_cached_parts.erase( part );
    }
}

void map::update_vehicle_list( submap *const to )
{
    // Update vehicle data
    for( auto & elem : to->vehicles ) {
        vehicle_list.insert( elem );
    }
}

void map::destroy_vehicle (vehicle *veh)
{
    if (!veh) {
        debugmsg("map::destroy_vehicle was passed NULL");
        return;
    }
    submap * const current_submap = get_submap_at_grid(veh->smx, veh->smy);
    for (size_t i = 0; i < current_submap->vehicles.size(); i++) {
        if (current_submap->vehicles[i] == veh) {
            vehicle_list.erase(veh);
            reset_vehicle_cache();
            current_submap->vehicles.erase (current_submap->vehicles.begin() + i);
            delete veh;
            return;
        }
    }
    debugmsg ("destroy_vehicle can't find it! name=%s, x=%d, y=%d", veh->name.c_str(), veh->smx, veh->smy);
}

void map::on_vehicle_moved() {
    set_outside_cache_dirty();
    set_transparency_cache_dirty();
}

void map::vehmove()
{
    // give vehicles movement points
    {
        VehicleList vehs = get_vehicles();
        for( auto &vehs_v : vehs ) {
            vehicle *veh = vehs_v.v;
            veh->gain_moves();
            veh->slow_leak();
        }
    }

    // 15 equals 3 >50mph vehicles, or up to 15 slow (1 square move) ones
    for( int count = 0; count < 15; count++ ) {
        if( !vehproceed() ) {
            break;
        } else {
            on_vehicle_moved();
        }
    }
    // Process item removal on the vehicles that were modified this turn.
    for( const auto &elem : dirty_vehicle_list ) {
        ( elem )->part_removal_cleanup();
    }
    dirty_vehicle_list.clear();
}

bool map::vehproceed()
{
    VehicleList vehs = get_vehicles();
    vehicle* veh = nullptr;
    float max_of_turn = 0;
    int x; int y;
    for( auto &vehs_v : vehs ) {
        if( vehs_v.v->of_turn > max_of_turn ) {
            veh = vehs_v.v;
            x = vehs_v.x;
            y = vehs_v.y;
            max_of_turn = veh->of_turn;
        }
    }
    if(!veh) { return false; }

    if (!inbounds(x, y)) {
        dbg( D_INFO ) << "stopping out-of-map vehicle. (x,y)=(" << x << "," << y << ")";
        veh->stop();
        veh->of_turn = 0;
        return true;
    }

    bool pl_ctrl = veh->player_in_control(&g->u);

    // k slowdown first.
    int slowdown = veh->skidding? 200 : 20; // mph lost per tile when coasting
    float kslw = (0.1 + veh->k_dynamics()) / ((0.1) + veh->k_mass());
    slowdown = (int) ceil(kslw * slowdown);
    if (abs(slowdown) > abs(veh->velocity)) {
        veh->stop();
    } else if (veh->velocity < 0) {
      veh->velocity += slowdown;
    } else {
      veh->velocity -= slowdown;
    }

    //low enough for bicycles to go in reverse.
    if (veh->velocity && abs(veh->velocity) < 20) {
        veh->stop();
    }

    if(veh->velocity == 0) {
        veh->of_turn -= .321f;
        return true;
    }

    std::vector<int> float_indices = veh->all_parts_with_feature(VPFLAG_FLOATS, false);
    if (float_indices.empty()) {
        // sink in water?
        std::vector<int> wheel_indices = veh->all_parts_with_feature(VPFLAG_WHEEL, false);
        int num_wheels = wheel_indices.size(), submerged_wheels = 0;
        for (int w = 0; w < num_wheels; w++) {
            const int p = wheel_indices[w];
            const int px = x + veh->parts[p].precalc[0].x;
            const int py = y + veh->parts[p].precalc[0].y;
            // deep water
            if (ter_at(px, py).has_flag(TFLAG_DEEP_WATER)) {
                submerged_wheels++;
            }
        }
        // submerged wheels threshold is 2/3.
        if (num_wheels && (float)submerged_wheels / num_wheels > .666) {
            add_msg(m_bad, _("Your %s sank."), veh->name.c_str());
            if( pl_ctrl ) {
                veh->unboard_all();
            }
            if( g->remoteveh() == veh ) {
                g->setremoteveh( nullptr );
            }
            // destroy vehicle (sank to nowhere)
            destroy_vehicle(veh);
            return true;
        }
    } else {

        int num = float_indices.size(), moored = 0;
        for (int w = 0; w < num; w++) {
            const int p = float_indices[w];
            const int px = x + veh->parts[p].precalc[0].x;
            const int py = y + veh->parts[p].precalc[0].y;

            if (!has_flag("SWIMMABLE", px, py)) {
                moored++;
            }
        }

        if (moored > num - 1) {
            veh->stop();
            veh->of_turn = 0;

            add_msg(m_info, _("Your %s is beached."), veh->name.c_str());

            return true;
        }

    }
    // One-tile step take some of movement
    //  terrain cost is 1000 on roads.
    // This is stupid btw, it makes veh magically seem
    //  to accelerate when exiting rubble areas.
    float ter_turn_cost = 500.0 * move_cost_ter_furn (x,y) / abs(veh->velocity);

    //can't afford it this turn?
    if(ter_turn_cost >= veh->of_turn) {
        veh->of_turn_carry = veh->of_turn;
        veh->of_turn = 0;
        return true;
    }

    veh->of_turn -= ter_turn_cost;

    // if not enough wheels, mess up the ground a bit.
    if (!veh->valid_wheel_config()) {
        veh->velocity += veh->velocity < 0 ? 2000 : -2000;
        for (auto &p : veh->parts) {
            const int px = x + p.precalc[0].x;
            const int py = y + p.precalc[0].y;
            const ter_id &pter = ter(px, py);
            if (pter == t_dirt || pter == t_grass) {
                ter_set(px, py, t_dirtmound);
            }
        }
    }

    if (veh->skidding) {
        if (one_in(4)) { // might turn uncontrollably while skidding
            veh->turn (one_in(2) ? -15 : 15);
        }
    }
    else if (pl_ctrl && rng(0, 4) > g->u.skillLevel("driving") && one_in(20)) {
        add_msg(m_warning, _("You fumble with the %s's controls."), veh->name.c_str());
        veh->turn (one_in(2) ? -15 : 15);
    }
    // eventually send it skidding if no control
    if (!veh->boarded_parts().size() && one_in (10)) {
        veh->skidding = true;
    }
    tileray mdir; // the direction we're moving
    if (veh->skidding) { // if skidding, it's the move vector
        mdir = veh->move;
    } else if (veh->turn_dir != veh->face.dir()) {
        mdir.init (veh->turn_dir); // driver turned vehicle, get turn_dir
    } else {
      mdir = veh->face;          // not turning, keep face.dir
    }
    mdir.advance (veh->velocity < 0? -1 : 1);
    const int dx = mdir.dx();           // where do we go
    const int dy = mdir.dy();           // where do we go
    bool can_move = true;
    // calculate parts' mount points @ next turn (put them into precalc[1])
    veh->precalc_mounts(1, veh->skidding ? veh->turn_dir : mdir.dir());

    int dmg_1 = 0;

    std::vector<veh_collision> veh_veh_colls;
    std::vector<veh_collision> veh_misc_colls;

    if (veh->velocity == 0) { can_move = false; }
    // find collisions
    int vel1 = veh->velocity/100; //velocity of car before collision
    veh->collision( veh_veh_colls, veh_misc_colls, dx, dy, can_move, dmg_1 );

    bool veh_veh_coll_flag = false;
    // Used to calculate the epicenter of the collision.
    point epicenter1(0, 0);
    point epicenter2(0, 0);

    if(veh_veh_colls.size()) { // we have dynamic crap!
        // effects of colliding with another vehicle:
        // transfers of momentum, skidding,
        // parts are damaged/broken on both sides,
        // remaining times are normalized,
        veh_veh_coll_flag = true;
        veh_collision c = veh_veh_colls[0]; //Note: What´s with collisions with more than 2 vehicles?
        vehicle* veh2 = (vehicle*) c.target;
        add_msg(m_bad, _("The %1$s's %2$s collides with the %3$s's %4$s."),
                       veh->name.c_str(),  veh->part_info(c.part).name.c_str(),
                       veh2->name.c_str(), veh2->part_info(c.target_part).name.c_str());

        // for reference, a cargo truck weighs ~25300, a bicycle 690,
        //  and 38mph is 3800 'velocity'
        rl_vec2d velo_veh1 = veh->velo_vec();
        rl_vec2d velo_veh2 = veh2->velo_vec();
        float m1 = veh->total_mass();
        float m2 = veh2->total_mass();
        //Energy of vehicle1 annd vehicle2 before collision
        float E = 0.5 * m1 * velo_veh1.norm() * velo_veh1.norm() +
            0.5 * m2 * velo_veh2.norm() * velo_veh2.norm();

        //collision_axis
        int x_cof1 = 0, y_cof1 = 0, x_cof2 = 0, y_cof2 = 0;
        veh ->center_of_mass(x_cof1, y_cof1);
        veh2->center_of_mass(x_cof2, y_cof2);
        rl_vec2d collision_axis_y;

        collision_axis_y.x = ( veh->global_x() + x_cof1 ) -  ( veh2->global_x() + x_cof2 );
        collision_axis_y.y = ( veh->global_y() + y_cof1 ) -  ( veh2->global_y() + y_cof2 );
        collision_axis_y = collision_axis_y.normalized();
        rl_vec2d collision_axis_x = collision_axis_y.get_vertical();
        // imp? & delta? & final? reworked:
        // newvel1 =( vel1 * ( mass1 - mass2 ) + ( 2 * mass2 * vel2 ) ) / ( mass1 + mass2 )
        // as per http://en.wikipedia.org/wiki/Elastic_collision
        //velocity of veh1 before collision in the direction of collision_axis_y
        float vel1_y = collision_axis_y.dot_product(velo_veh1);
        float vel1_x = collision_axis_x.dot_product(velo_veh1);
        //velocity of veh2 before collision in the direction of collision_axis_y
        float vel2_y = collision_axis_y.dot_product(velo_veh2);
        float vel2_x = collision_axis_x.dot_product(velo_veh2);
        // e = 0 -> inelastic collision
        // e = 1 -> elastic collision
        float e = get_collision_factor(vel1_y/100 - vel2_y/100);

        //velocity after collision
        float vel1_x_a = vel1_x;
        // vel1_x_a = vel1_x, because in x-direction we have no transmission of force
        float vel2_x_a = vel2_x;
        //transmission of force only in direction of collision_axix_y
        //equation: partially elastic collision
        float vel1_y_a = ( m2 * vel2_y * ( 1 + e ) + vel1_y * ( m1 - m2 * e) ) / ( m1 + m2);
        //equation: partially elastic collision
        float vel2_y_a = ( m1 * vel1_y * ( 1 + e ) + vel2_y * ( m2 - m1 * e) ) / ( m1 + m2);
        //add both components; Note: collision_axis is normalized
        rl_vec2d final1 = collision_axis_y * vel1_y_a + collision_axis_x * vel1_x_a;
        //add both components; Note: collision_axis is normalized
        rl_vec2d final2 = collision_axis_y * vel2_y_a + collision_axis_x * vel2_x_a;

        //Energy after collision
        float E_a = 0.5 * m1 * final1.norm() * final1.norm() +
            0.5 * m2 * final2.norm() * final2.norm();
        float d_E = E - E_a;  //Lost energy at collision -> deformation energy
        float dmg = std::abs( d_E / 1000 / 2000 );  //adjust to balance damage
        float dmg_veh1 = dmg * 0.5;
        float dmg_veh2 = dmg * 0.5;

        int coll_parts_cnt = 0; //quantity of colliding parts between veh1 and veh2
        for( auto &veh_veh_coll : veh_veh_colls ) {
            veh_collision tmp_c = veh_veh_coll;
            if(veh2 == (vehicle*) tmp_c.target) { coll_parts_cnt++; }
        }

        float dmg1_part = dmg_veh1 / coll_parts_cnt;
        float dmg2_part = dmg_veh2 / coll_parts_cnt;

        //damage colliding parts (only veh1 and veh2 parts)
        for( auto &veh_veh_coll : veh_veh_colls ) {
            veh_collision tmp_c = veh_veh_coll;

            if(veh2 == (vehicle*) tmp_c.target) {
                int parm1 = veh->part_with_feature (tmp_c.part, VPFLAG_ARMOR);
                if (parm1 < 0) {
                    parm1 = tmp_c.part;
                }
                int parm2 = veh2->part_with_feature (tmp_c.target_part, VPFLAG_ARMOR);
                if (parm2 < 0) {
                    parm2 = tmp_c.target_part;
                }
                epicenter1.x += veh->parts[parm1].mount.x;
                epicenter1.y += veh->parts[parm1].mount.y;
                veh->damage(parm1, dmg1_part, 1);

                epicenter2.x += veh2->parts[parm2].mount.x;
                epicenter2.y += veh2->parts[parm2].mount.y;
                veh2->damage(parm2, dmg2_part, 1);
            }
        }
        epicenter1.x /= coll_parts_cnt;
        epicenter1.y /= coll_parts_cnt;
        epicenter2.x /= coll_parts_cnt;
        epicenter2.y /= coll_parts_cnt;


        if (dmg2_part > 100) {
            // shake veh because of collision
            veh2->damage_all(dmg2_part / 2, dmg2_part, 1, epicenter2);
        }

        dmg_1 += dmg1_part;

        veh->move.init (final1.x, final1.y);
        veh->velocity = final1.norm();
        // shrug it off if the change is less than 8mph.
        if(dmg_veh1 > 800) {
            veh->skidding = 1;
        }
        veh2->move.init(final2.x, final2.y);
        veh2->velocity = final2.norm();
        if(dmg_veh2 > 800) {
            veh2->skidding = 1;
        }
        //give veh2 the initiative to proceed next before veh1
        float avg_of_turn = (veh2->of_turn + veh->of_turn) / 2;
        if(avg_of_turn < .1f)
            avg_of_turn = .1f;
        veh->of_turn = avg_of_turn * .9;
        veh2->of_turn = avg_of_turn * 1.1;
    }

    for( auto &veh_misc_coll : veh_misc_colls ) {

        const point collision_point = veh->parts[veh_misc_coll.part].mount;
        int coll_dmg = veh_misc_coll.imp;
        //Shock damage
        veh->damage_all(coll_dmg / 2, coll_dmg, 1, collision_point);
    }

    int coll_turn = 0;
    if (dmg_1 > 0) {
        int vel1_a = veh->velocity / 100; //velocity of car after collision
        int d_vel = abs(vel1 - vel1_a);

        std::vector<int> ppl = veh->boarded_parts();

        for (auto &ps : ppl) {
            player *psg = veh->get_passenger (ps);
            if (!psg) {
                debugmsg ("throw passenger: empty passenger at part %d", ps);
                continue;
            }

            bool throw_from_seat = 0;
            if (veh->part_with_feature (ps, VPFLAG_SEATBELT) == -1) {
                throw_from_seat = d_vel * rng(80, 120) / 100 > (psg->str_cur * 1.5 + 5);
            }

            //damage passengers if d_vel is too high
            if(d_vel > 60* rng(50,100)/100 && !throw_from_seat) {
                int dmg = d_vel/4*rng(70,100)/100;
                psg->hurtall(dmg, nullptr);
                if (psg == &g->u) {
                    add_msg(m_bad, _("You take %d damage by the power of the impact!"), dmg);
                } else if (psg->name.length()) {
                    add_msg(m_bad, _("%s takes %d damage by the power of the impact!"),
                                   psg->name.c_str(), dmg);
                }
            }

            if (throw_from_seat) {
                if (psg == &g->u) {
                    add_msg(m_bad, _("You are hurled from the %s's seat by the power of the impact!"),
                                   veh->name.c_str());
                } else if (psg->name.length()) {
                    add_msg(m_bad, _("%s is hurled from the %s's seat by the power of the impact!"),
                                   psg->name.c_str(), veh->name.c_str());
                }
                unboard_vehicle(x + veh->parts[ps].precalc[0].x,
                                     y + veh->parts[ps].precalc[0].y);
                g->fling_creature(psg, mdir.dir() + rng(0, 60) - 30,
                                           (vel1 - psg->str_cur < 10 ? 10 :
                                            vel1 - psg->str_cur));
            } else if (veh->part_with_feature (ps, "CONTROLS") >= 0) {
                // FIXME: should actually check if passenger is in control,
                // not just if there are controls there.
                const int lose_ctrl_roll = rng (0, dmg_1);
                if (lose_ctrl_roll > psg->dex_cur * 2 + psg->skillLevel("driving") * 3) {
                    if (psg == &g->u) {
                        add_msg(m_warning, _("You lose control of the %s."), veh->name.c_str());
                    } else if (psg->name.length()) {
                        add_msg(m_warning, _("%s loses control of the %s."), psg->name.c_str());
                    }
                    int turn_amount = (rng (1, 3) * sqrt((double)vel1_a) / 2) / 15;
                    if (turn_amount < 1) {
                        turn_amount = 1;
                    }
                    turn_amount *= 15;
                    if (turn_amount > 120) {
                        turn_amount = 120;
                    }
                    coll_turn = one_in (2)? turn_amount : -turn_amount;
                }
            }
        }
    }
    if(veh_veh_coll_flag) return true;

    // now we're gonna handle traps we're standing on (if we're still moving).
    // this is done here before displacement because
    // after displacement veh reference would be invdalid.
    // damn references!
    if (can_move) {
        std::vector<int> wheel_indices = veh->all_parts_with_feature("WHEEL", false);
        for (auto &w : wheel_indices) {
            const int wheel_x = x + veh->parts[w].precalc[0].x;
            const int wheel_y = y + veh->parts[w].precalc[0].y;
            if (one_in(2)) {
                if( displace_water( wheel_x, wheel_y) && pl_ctrl ) {
                    add_msg(m_warning, _("You hear a splash!"));
                }
            }
            veh->handle_trap( wheel_x, wheel_y, w );
            if( !has_flag( "SEALED", wheel_x, wheel_y ) ) {
            auto item_vec = i_at( wheel_x, wheel_y );
            for( auto it = item_vec.begin(); it != item_vec.end(); ) {
                it->damage += rng( 0, 3 );
                if( it->damage > 4 ) {
                    it = item_vec.erase(it);
                } else {
                    ++it;
                }
            }
            }
        }
    }

    int last_turn_dec = 1;
    if (veh->last_turn < 0) {
        veh->last_turn += last_turn_dec;
        if (veh->last_turn > -last_turn_dec) { veh->last_turn = 0; }
    } else if (veh->last_turn > 0) {
        veh->last_turn -= last_turn_dec;
        if (veh->last_turn < last_turn_dec) { veh->last_turn = 0; }
    }

    if (can_move) {
        // accept new direction
        if (veh->skidding) {
            veh->face.init (veh->turn_dir);
            if(pl_ctrl) {
                veh->possibly_recover_from_skid();
            }
        } else {
            veh->face = mdir;
        }
        veh->move = mdir;
        if (coll_turn) {
            veh->skidding = true;
            veh->turn (coll_turn);
        }
        // accept new position
        // if submap changed, we need to process grid from the beginning.
        displace_vehicle (x, y, dx, dy);
    } else { // can_move
        veh->stop();
    }
    // If the PC is in the currently moved vehicle, adjust the
    // view offset.
    if (g->u.controlling_vehicle && veh_at(g->u.posx(), g->u.posy()) == veh) {
        g->calc_driving_offset(veh);
    }
    // redraw scene
    g->draw();
    return true;
}

// 2D vehicle functions

VehicleList map::get_vehicles(const int sx, const int sy, const int ex, const int ey)
{
    return get_vehicles( tripoint( sx, sy, abs_sub.z ), tripoint( ex, ey, abs_sub.z ) );
}

vehicle* map::veh_at(const int x, const int y, int &part_num)
{
    if( !veh_in_active_range || !INBOUNDS(x, y) ) {
        return nullptr; // Out-of-bounds - null vehicle
    }

    // Apparently this is a proper coding practice and not an ugly hack
    return const_cast<vehicle *>( veh_at_internal( x, y, part_num ) );
}

const vehicle* map::veh_at(const int x, const int y, int &part_num) const
{
    if( !veh_in_active_range || !INBOUNDS( x, y ) ) {
        return nullptr; // Out-of-bounds - null vehicle
    }

    return veh_at_internal( x, y, part_num );
}

const vehicle* map::veh_at_internal( const int x, const int y, int &part_num) const
{
    // This function is called A LOT. Move as much out of here as possible.
    if( !veh_in_active_range || !veh_exists_at[x][y] ) {
        return nullptr; // Clear cache indicates no vehicle. This should optimize a great deal.
    }

    const auto it = veh_cached_parts.find( point( x, y ) );
    if( it != veh_cached_parts.end() ) {
        part_num = it->second.second;
        return it->second.first;
    }

    debugmsg( "vehicle part cache indicated vehicle not found: %d %d", x, y );
    return nullptr;
}

vehicle* map::veh_at(const int x, const int y)
{
    int part = 0;
    return veh_at(x, y, part);
}

const vehicle* map::veh_at(const int x, const int y) const
{
    int part = 0;
    return veh_at(x, y, part);
}

point map::veh_part_coordinates(const int x, const int y)
{
    int part_num;
    vehicle* veh = veh_at(x, y, part_num);

    if(veh == nullptr) {
        return point(0,0);
    }

    return veh->parts[part_num].mount;
}

void map::board_vehicle(int x, int y, player *p)
{
    board_vehicle( tripoint( x, y, abs_sub.z ), p );
}

void map::unboard_vehicle(const int x, const int y)
{
    unboard_vehicle( tripoint( x, y, abs_sub.z ) );
}

bool map::displace_vehicle (int &x, int &y, const int dx, const int dy, bool test)
{
    tripoint p( x, y, abs_sub.z );
    tripoint dp( dx, dy, 0 );
    bool ret = displace_vehicle( p, dp, test );
    x = p.x;
    y = p.y;
    return ret;
}

bool map::displace_water (const int x, const int y)
{
    return displace_water( tripoint( x, y, abs_sub.z ) );
}

// 3D vehicle functions

VehicleList map::get_vehicles( const tripoint &start, const tripoint &end )
{
    const int chunk_sx = std::max( 0, (start.x / SEEX) - 1 );
    const int chunk_ex = std::min( my_MAPSIZE - 1, (end.x / SEEX) + 1 );
    const int chunk_sy = std::max( 0, (start.y / SEEY) - 1 );
    const int chunk_ey = std::min( my_MAPSIZE - 1, (end.y / SEEY) + 1 );
    const int chunk_sz = start.z;
    const int chunk_ez = end.z;
    VehicleList vehs;

    for( int cx = chunk_sx; cx <= chunk_ex; ++cx ) {
        for( int cy = chunk_sy; cy <= chunk_ey; ++cy ) {
            for( int cz = chunk_sz; cz <= chunk_ez; ++cz ) {
                submap *current_submap = get_submap_at_grid( cx, cy, cz );
                for( auto &elem : current_submap->vehicles ) {
                    wrapped_vehicle w;
                    w.v = elem;
                    w.x = w.v->posx + cx * SEEX;
                    w.y = w.v->posy + cy * SEEY;
                    w.z = cz;
                    w.i = cx;
                    w.j = cy;
                    vehs.push_back( w );
                }
            }
        }
    }

    return vehs;
}

vehicle* map::veh_at( const tripoint &p, int &part_num )
{
    if( !veh_in_active_range || !inbounds( p ) ) {
        return nullptr; // Out-of-bounds - null vehicle
    }

    // Apparently this is a proper coding practice and not an ugly hack
    return const_cast<vehicle *>( veh_at_internal( p, part_num ) );
}

const vehicle* map::veh_at( const tripoint &p, int &part_num ) const
{
    if( !veh_in_active_range || !inbounds( p ) ) {
        return nullptr; // Out-of-bounds - null vehicle
    }

    return veh_at_internal( p, part_num );
}

const vehicle* map::veh_at_internal( const tripoint &p, int &part_num ) const
{
    // This function is called A LOT. Move as much out of here as possible.
    if( !veh_in_active_range || !veh_exists_at[p.x][p.y] ) {
        return nullptr; // Clear cache indicates no vehicle. This should optimize a great deal.
    }

    const auto it = veh_cached_parts.find( point( p.x, p.y ) );
    if( it != veh_cached_parts.end() ) {
        part_num = it->second.second;
        return it->second.first;
    }

    debugmsg( "vehicle part cache indicated vehicle not found: %d %d %d", p.x, p.y, p.z );
    return nullptr;
}

vehicle* map::veh_at( const tripoint &p )
{
    int part = 0;
    return veh_at( p, part );
}

const vehicle* map::veh_at( const tripoint &p ) const
{
    int part = 0;
    return veh_at( p, part );
}

point map::veh_part_coordinates( const tripoint &p )
{
    int part_num;
    vehicle* veh = veh_at( p, part_num );

    if(veh == nullptr) {
        return point( 0,0 );
    }

    return veh->parts[part_num].mount;
}

void map::board_vehicle( const tripoint &pos, player *p )
{
    if( !p ) {
        debugmsg ("map::board_vehicle: null player");
        return;
    }

    int part = 0;
    vehicle *veh = veh_at( pos, part );
    if( !veh ) {
        if( p->grab_point.x == 0 && p->grab_point.y == 0 ) {
            debugmsg ("map::board_vehicle: vehicle not found");
        }
        return;
    }

    const int seat_part = veh->part_with_feature (part, VPFLAG_BOARDABLE);
    if( seat_part < 0 ) {
        debugmsg( "map::board_vehicle: boarding %s (not boardable)",
                  veh->part_info(part).name.c_str() );
        return;
    }
    if( veh->parts[seat_part].has_flag( vehicle_part::passenger_flag ) ) {
        player *psg = veh->get_passenger( seat_part );
        debugmsg( "map::board_vehicle: passenger (%s) is already there",
                  psg ? psg->name.c_str() : "<null>" );
        unboard_vehicle( pos );
    }
    veh->parts[seat_part].set_flag(vehicle_part::passenger_flag);
    veh->parts[seat_part].passenger_id = p->getID();

    p->setx( pos.x );
    p->sety( pos.y );
    p->setz( pos.z );
    p->in_vehicle = true;
    if( p == &g->u &&
        ( pos.x < SEEX * int(my_MAPSIZE / 2) ||
          pos.y < SEEY * int(my_MAPSIZE / 2) ||
          pos.x >= SEEX * (1 + int(my_MAPSIZE / 2) ) ||
          pos.y >= SEEY * (1 + int(my_MAPSIZE / 2) ) ) ) {
        int tempx = pos.x;
        int tempy = pos.y;
        g->update_map( tempx, tempy );
    }
}

void map::unboard_vehicle( const tripoint &p )
{
    int part = 0;
    vehicle *veh = veh_at( p, part );
    player *passenger = nullptr;
    if( !veh ) {
        debugmsg ("map::unboard_vehicle: vehicle not found");
        // Try and force unboard the player anyway.
        if( g->u.pos3() == p ) {
            passenger = &(g->u);
        } else {
            int npcdex = g->npc_at( p.x, p.y );
            if( npcdex != -1 ) {
                passenger = g->active_npc[npcdex];
            }
        }
        if( passenger ) {
            passenger->in_vehicle = false;
            passenger->driving_recoil = 0;
            passenger->controlling_vehicle = false;
        }
        return;
    }
    const int seat_part = veh->part_with_feature( part, VPFLAG_BOARDABLE, false );
    if( seat_part < 0 ) {
        debugmsg ("map::unboard_vehicle: unboarding %s (not boardable)",
                  veh->part_info(part).name.c_str());
        return;
    }
    passenger = veh->get_passenger(seat_part);
    if( !passenger ) {
        debugmsg ("map::unboard_vehicle: passenger not found");
        return;
    }
    passenger->in_vehicle = false;
    passenger->driving_recoil = 0;
    passenger->controlling_vehicle = false;
    veh->parts[seat_part].remove_flag(vehicle_part::passenger_flag);
    veh->skidding = true;
}

bool map::displace_vehicle( tripoint &p, const tripoint &dp, bool test )
{
    const tripoint p2 = p + dp;
    tripoint src = p;
    tripoint dst = p2;

    if( !inbounds( src ) ) {
        add_msg( m_debug, "map::displace_vehicle: coords out of bounds %d,%d,%d->%d,%d,%d",
                        src.x, src.y, src.z, dst.x, dst.y, dst.z );
        return false;
    }

    int src_offset_x, src_offset_y, dst_offset_x, dst_offset_y;
    submap *const src_submap = get_submap_at( src, src_offset_x, src_offset_y );
    submap *const dst_submap = get_submap_at( dst, dst_offset_x, dst_offset_y );

    if( test ) {
        return src_submap != dst_submap;
    }

    // first, let's find our position in current vehicles vector
    int our_i = -1;
    for( size_t i = 0; i < src_submap->vehicles.size(); i++ ) {
        if( src_submap->vehicles[i]->posx == src_offset_x &&
              src_submap->vehicles[i]->posy == src_offset_y ) {
            our_i = i;
            break;
        }
    }
    if( our_i < 0 ) {
        vehicle *v = veh_at( p );
        for( auto & smap : grid ) {
            for (size_t i = 0; i < smap->vehicles.size(); i++) {
                if (smap->vehicles[i] == v) {
                    our_i = i;
                    const_cast<submap*&>(src_submap) = smap;
                    break;
                }
            }
        }
    }
    if( our_i < 0 ) {
        add_msg( m_debug, "displace_vehicle our_i=%d", our_i );
        return false;
    }
    // move the vehicle
    vehicle *veh = src_submap->vehicles[our_i];
    // don't let it go off grid
    if( !inbounds( p2 ) ) {
        veh->stop();
        // Silent debug
        dbg(D_ERROR) << "map:displace_vehicle: Stopping vehicle, displaced dp=("
                     << dp.x << ", " << dp.y << ", " << dp.z << ")";
        return false;
    }

    // record every passenger inside
    std::vector<int> psg_parts = veh->boarded_parts();
    std::vector<player *> psgs;
    for( auto &prt : psg_parts ) {
        psgs.push_back( veh->get_passenger( prt ) );
    }

    const int rec = abs( veh->velocity ) / 5 / 100;

    bool need_update = false;
    int upd_x, upd_y;
    // move passengers
    for( size_t i = 0; i < psg_parts.size(); i++ ) {
        player *psg = psgs[i];
        const int prt = psg_parts[i];
        if( !psg ) {
            debugmsg( "empty passenger part %d pcoord=%d,%d,%d u=%d,%d,%d?",
                         prt,
                         veh->global_x() + veh->parts[prt].precalc[0].x,
                         veh->global_y() + veh->parts[prt].precalc[0].y,
                         p.z,
                         g->u.posx(), g->u.posy(), g->u.posz() );
            veh->parts[prt].remove_flag(vehicle_part::passenger_flag);
            continue;
        }
        // add recoil
        psg->driving_recoil = rec;
        // displace passenger taking in account vehicle movement (dx, dy)
        // and turning: precalc[0] contains previous frame direction,
        // and precalc[1] should contain next direction
        psg->setx( psg->posx() + dp.x + veh->parts[prt].precalc[1].x - veh->parts[prt].precalc[0].x );
        psg->sety( psg->posy() + dp.y + veh->parts[prt].precalc[1].y - veh->parts[prt].precalc[0].y );
        if( psg == &g->u ) { // if passenger is you, we need to update the map
            need_update = true;
            upd_x = psg->posx();
            upd_y = psg->posy();
        }
    }

    veh->shed_loose_parts();
    for( auto &prt : veh->parts ) {
        prt.precalc[0] = prt.precalc[1];
    }

    veh->posx = dst_offset_x;
    veh->posy = dst_offset_y;
    if (src_submap != dst_submap) {
        veh->set_submap_moved( int( p2.x / SEEX ), int( p2.y / SEEY ) );
        dst_submap->vehicles.push_back( veh );
        src_submap->vehicles.erase( src_submap->vehicles.begin() + our_i );
    }

    // Need old coords to check for remote control
    bool remote = veh->remote_controlled( &g->u );

    p += dp;

    update_vehicle_cache(veh);

    bool was_update = false;
    if (need_update &&
          (upd_x < SEEX * int(my_MAPSIZE / 2) || upd_y < SEEY *int(my_MAPSIZE / 2) ||
          upd_x >= SEEX * (1+int(my_MAPSIZE / 2)) ||
          upd_y >= SEEY * (1+int(my_MAPSIZE / 2)))) {
        // map will shift, so adjust vehicle coords we've been passed
        if (upd_x < SEEX * int(my_MAPSIZE / 2)) {
            p.x += SEEX;
        } else if (upd_x >= SEEX * (1+int(my_MAPSIZE / 2))) {
            p.x -= SEEX;
        }
        if (upd_y < SEEY * int(my_MAPSIZE / 2)) {
            p.y += SEEY;
        } else if (upd_y >= SEEY * (1+int(my_MAPSIZE / 2))) {
            p.y -= SEEY;
        }
        g->update_map(upd_x, upd_y);
        was_update = true;
    }
    if( remote ) { // Has to be after update_map or coords won't be valid
        g->setremoteveh( veh );
    }

    return (src_submap != dst_submap) || was_update;
}

bool map::displace_water ( const tripoint &p )
{
    // Check for shallow water
    if( has_flag( "SWIMMABLE", p ) && !has_flag( TFLAG_DEEP_WATER, p ) ) {
        int dis_places = 0, sel_place = 0;
        for( int pass = 0; pass < 2; pass++ ) {
            // we do 2 passes.
            // first, count how many non-water places around
            // then choose one within count and fill it with water on second pass
            if( pass != 0 ) {
                sel_place = rng( 0, dis_places - 1 );
                dis_places = 0;
            }
            tripoint temp( p );
            int &tx = temp.x;
            int &ty = temp.y;
            for( tx = p.x - 1; tx <= p.x + 1; tx++ ) {
                for( ty = p.y -1; ty <= p.y + 1; ty++ ) {
                    if( ( tx != p.x && ty != p.y )
                            || move_cost_ter_furn( temp ) == 0
                            || has_flag( TFLAG_DEEP_WATER, temp ) ) {
                        continue;
                    }
                    ter_id ter0 = ter( temp );
                    if( ter0 == t_water_sh ||
                        ter0 == t_water_dp) {
                        continue;
                    }
                    if( pass != 0 && dis_places == sel_place ) {
                        ter_set( temp, t_water_sh );
                        ter_set( temp, t_dirt );
                        return true;
                    }

                    dis_places++;
                }
            }
        }
    }
    return false;
}

// End of 3D vehicle

// 2D overloads for furniture
// To be removed once not needed
void map::set(const int x, const int y, const ter_id new_terrain, const furn_id new_furniture)
{
    furn_set(x, y, new_furniture);
    ter_set(x, y, new_terrain);
}

void map::set(const int x, const int y, const std::string new_terrain, const std::string new_furniture) {
    furn_set(x, y, new_furniture);
    ter_set(x, y, new_terrain);
}

std::string map::name(const int x, const int y)
{
 return has_furn(x, y) ? furn_at(x, y).name : ter_at(x, y).name;
}

bool map::has_furn(const int x, const int y) const
{
  return furn(x, y) != f_null;
}

std::string map::get_furn(const int x, const int y) const
{
    return furn_at(x, y).id;
}

furn_t & map::furn_at(const int x, const int y) const
{
    return furnlist[ furn(x,y) ];
}

furn_id map::furn(const int x, const int y) const
{
    if( !INBOUNDS(x, y) ) {
        return f_null;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);

    return current_submap->get_furn(lx, ly);
}

void map::furn_set(const int x, const int y, const furn_id new_furniture)
{
 if (!INBOUNDS(x, y)) {
  return;
 }

 int lx, ly;
 submap * const current_submap = get_submap_at(x, y, lx, ly);

 set_transparency_cache_dirty();
 current_submap->set_furn(lx, ly, new_furniture);
}

void map::furn_set(const int x, const int y, const std::string new_furniture) {
    if ( furnmap.find(new_furniture) == furnmap.end() ) {
        return;
    }
    furn_set(x, y, (furn_id)furnmap[ new_furniture ].loadid );
}

std::string map::furnname(const int x, const int y) {
 return furn_at(x, y).name;
}
// End of 2D overloads for furniture

void map::set( const tripoint &p, const ter_id new_terrain, const furn_id new_furniture)
{
    furn_set( p, new_furniture );
    ter_set( p, new_terrain );
}

void map::set( const tripoint &p, const std::string new_terrain, const std::string new_furniture) {
    furn_set( p, new_furniture );
    ter_set( p, new_terrain );
}

std::string map::name( const tripoint &p )
{
 return has_furn( p ) ? furn_at( p ).name : ter_at( p ).name;
}

bool map::has_furn( const tripoint &p ) const
{
  return furn( p ) != f_null;
}

std::string map::get_furn( const tripoint &p ) const
{
    return furn_at( p ).id;
}

furn_t & map::furn_at( const tripoint &p ) const
{
    return furnlist[ furn( p ) ];
}

furn_id map::furn( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return f_null;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->get_furn( lx, ly );
}

void map::furn_set( const tripoint &p, const furn_id new_furniture)
{
    if (!inbounds( p )) {
        return;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    // set the dirty flags
    // TODO: consider checking if the transparency value actually changes
    set_transparency_cache_dirty();
    current_submap->set_furn( lx, ly, new_furniture );
}

void map::furn_set( const tripoint &p, const std::string new_furniture) {
    if( furnmap.find(new_furniture) == furnmap.end() ) {
        return;
    }

    furn_set( p, (furn_id)furnmap[ new_furniture ].loadid );
}

bool map::can_move_furniture( const tripoint &pos, player *p ) {
    furn_t furniture_type = furn_at( pos );
    int required_str = furniture_type.move_str_req;

    // Object can not be moved (or nothing there)
    if( required_str < 0 ) { 
        return false;
    }

    if( p != nullptr && p->str_cur < required_str ) {
        return false;
    }

    return true;
}

std::string map::furnname( const tripoint &p ) {
    return furn_at( p ).name;
}

// 2D overloads for terrain
// To be removed once not needed

ter_id map::ter(const int x, const int y) const
{
    if( !INBOUNDS(x, y) ) {
        return t_null;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);
    return current_submap->get_ter( lx, ly );
}

std::string map::get_ter(const int x, const int y) const {
    return ter_at(x, y).id;
}

std::string map::get_ter_harvestable(const int x, const int y) const {
    return ter_at(x, y).harvestable;
}

ter_id map::get_ter_transforms_into(const int x, const int y) const {
    return (ter_id)termap[ ter_at(x, y).transforms_into ].loadid;
}

int map::get_ter_harvest_season(const int x, const int y) const {
    return ter_at(x, y).harvest_season;
}

ter_t & map::ter_at(const int x, const int y) const
{
    return terlist[ ter(x,y) ];
}

void map::ter_set(const int x, const int y, const std::string new_terrain) {
    if ( termap.find(new_terrain) == termap.end() ) {
        return;
    }
    ter_set(x, y, (ter_id)termap[ new_terrain ].loadid );
}

void map::ter_set(const int x, const int y, const ter_id new_terrain) {
    if (!INBOUNDS(x, y)) {
        return;
    }

    set_transparency_cache_dirty();
    set_outside_cache_dirty();

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);
    current_submap->set_ter( lx, ly, new_terrain );
}

std::string map::tername(const int x, const int y) const
{
 return ter_at(x, y).name;
}
// End of 2D overloads for terrain

/*
 * Get the terrain integer id. This is -not- a number guaranteed to remain
 * the same across revisions; it is a load order, and can change when mods
 * are loaded or removed. The old t_floor style constants will still work but
 * are -not- guaranteed; if a mod removes t_lava, t_lava will equal t_null;
 * New terrains added to the core game generally do not need this, it's
 * retained for high performance comparisons, save/load, and gradual transition
 * to string terrain.id
 */
ter_id map::ter( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return t_null;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->get_ter( lx, ly );
}

/*
 * Get the terrain string id. This will remain the same across revisions,
 * unless a mod eliminates or changes it. Generally this is less efficient
 * than ter_id, but only an issue if thousands of comparisons are made.
 */
std::string map::get_ter( const tripoint &p ) const {
    return ter_at( p ).id;
}

/*
 * Get the terrain harvestable string (what will get harvested from the terrain)
 */
std::string map::get_ter_harvestable( const tripoint &p ) const {
    return ter_at( p ).harvestable;
}

/*
 * Get the terrain transforms_into id (what will the terrain transforms into)
 */
ter_id map::get_ter_transforms_into( const tripoint &p ) const {
    return (ter_id)termap[ ter_at( p ).transforms_into ].loadid;
}

/*
 * Get the harvest season from the terrain
 */
int map::get_ter_harvest_season( const tripoint &p ) const {
    return ter_at( p ).harvest_season;
}

/*
 * Get a reference to the actual terrain struct.
 */
ter_t & map::ter_at( const tripoint &p ) const
{
    return terlist[ ter( p ) ];
}

/*
 * set terrain via string; this works for -any- terrain id
 */
void map::ter_set( const tripoint &p, const std::string new_terrain) {
    if(  termap.find(new_terrain) == termap.end() ) {
        return;
    }

    ter_set( p, (ter_id)termap[ new_terrain ].loadid );
}

/*
 * set terrain via builtin t_keyword; only if defined, and will not work
 * for mods
 */
void map::ter_set( const tripoint &p, const ter_id new_terrain) {
    if( !inbounds( p ) ) {
        return;
    }

    // set the dirty flags
    // TODO: consider checking if the transparency value actually changes
    set_transparency_cache_dirty();
    set_outside_cache_dirty();

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    current_submap->set_ter( lx, ly, new_terrain );
}

std::string map::tername( const tripoint &p ) const
{
 return ter_at( p ).name;
}

std::string map::features(const int x, const int y)
{
    // This is used in an info window that is 46 characters wide, and is expected
    // to take up one line.  So, make sure it does that.
    // FIXME: can't control length of localized text.
    // Make the caller wrap properly, if it does not already.
    std::string ret;
    if (is_bashable(x, y)) {
        ret += _("Smashable. ");
    }
    if (has_flag("DIGGABLE", x, y)) {
        ret += _("Diggable. ");
    }
    if (has_flag("ROUGH", x, y)) {
        ret += _("Rough. ");
    }
    if (has_flag("UNSTABLE", x, y)) {
        ret += _("Unstable. ");
    }
    if (has_flag("SHARP", x, y)) {
        ret += _("Sharp. ");
    }
    if (has_flag("FLAT", x, y)) {
        ret += _("Flat. ");
    }
    return ret;
}

int map::move_cost_internal(const furn_t &furniture, const ter_t &terrain, const vehicle *veh, const int vpart) const
{
    if( terrain.movecost == 0 || ( furniture.loadid != f_null && furniture.movecost < 0 ) ) {
        return 0;
    }

    if( veh != nullptr ) {
        if( veh->obstacle_at_part( vpart ) >= 0 ) {
            return 0;
        } else {
            const int ipart = veh->part_with_feature( vpart, VPFLAG_AISLE );
            if( ipart >= 0 ) {
                return 2;
            }

            return 8;
        }
    }

    if( furniture.loadid != f_null ) {
        return std::max( terrain.movecost + furniture.movecost, 0 );
    }

    return std::max( terrain.movecost, 0 );
}

// Move cost: 2D overloads

int map::move_cost(const int x, const int y, const vehicle *ignored_vehicle) const
{
    if( !INBOUNDS( x, y ) ) {
        return 0;
    }

    int part;
    const furn_t &furniture = furn_at( x, y );
    const ter_t &terrain = ter_at( x, y );
    const vehicle *veh = veh_at( x, y, part );
    if( veh == ignored_vehicle ) {
        veh = nullptr;
    }

    return move_cost_internal( furniture, terrain, veh, part );
}

int map::move_cost_ter_furn(const int x, const int y) const
{
    if (!INBOUNDS(x, y)) {
        return 0;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);

    const int tercost = terlist[ current_submap->get_ter( lx, ly ) ].movecost;
    if ( tercost == 0 ) {
        return 0;
    }

    const int furncost = furnlist[ current_submap->get_furn(lx, ly) ].movecost;
    if ( furncost < 0 ) {
        return 0;
    }

    const int cost = tercost + furncost;
    return cost > 0 ? cost : 0;
}

int map::combined_movecost(const int x1, const int y1,
                           const int x2, const int y2,
                           const vehicle *ignored_vehicle, const int modifier) const
{
    int cost1 = move_cost(x1, y1, ignored_vehicle);
    int cost2 = move_cost(x2, y2, ignored_vehicle);
    // 50 moves taken per move_cost (70.71.. diagonally)
    int mult = (trigdist && x1 != x2 && y1 != y2 ? 71 : 50);
    return (cost1 + cost2 + modifier) * mult / 2;
}

// Move cost: 3D

int map::move_cost( const tripoint &p, const vehicle *ignored_vehicle ) const
{
    if( !inbounds( p ) ) {
        return 0;
    }

    int part;
    const furn_t &furniture = furn_at( p );
    const ter_t &terrain = ter_at( p );
    const vehicle *veh = veh_at( p, part );
    if( veh == ignored_vehicle ) {
        veh = nullptr;
    }

    return move_cost_internal( furniture, terrain, veh, part );
}

int map::move_cost_ter_furn( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return 0;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    const int tercost = terlist[ current_submap->get_ter( lx, ly ) ].movecost;
    if ( tercost == 0 ) {
        return 0;
    }

    const int furncost = furnlist[ current_submap->get_furn( lx, ly ) ].movecost;
    if ( furncost < 0 ) {
        return 0;
    }

    const int cost = tercost + furncost;
    return cost > 0 ? cost : 0;
}

int map::combined_movecost( const tripoint &from, const tripoint &to,
                            const vehicle *ignored_vehicle, const int modifier ) const
{
    const int mults[4] = { 0, 50, 71, 100 };
    int cost1 = move_cost( from, ignored_vehicle );
    int cost2 = move_cost( to, ignored_vehicle );
    // Multiply cost depending on the number of differing axes
    // 0 if all axes are equal, 100% if only 1 differs, 141% for 2, 200% for 3
    size_t match = ( from.x != to.x ) + ( from.y != to.y ) + ( from.z != to.z );
    return (cost1 + cost2 + modifier) * mults[match] / 2;
}

// End of move cost

// 2D flags

bool map::has_flag(const std::string &flag, const int x, const int y) const
{
    static const std::string flag_str_REDUCE_SCENT("REDUCE_SCENT"); // construct once per runtime, slash delay 90%
    if (!INBOUNDS(x, y)) {
        return false;
    }

    int vpart;
    const vehicle *veh = veh_at( x, y, vpart );
    if( veh != nullptr && flag_str_REDUCE_SCENT == flag && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    return has_flag_ter_or_furn(flag, x, y);
}

bool map::can_put_items(const int x, const int y)
{
    return !has_flag("NOITEM", x, y) && !has_flag("SEALED", x, y);
}

bool map::has_flag_ter(const std::string & flag, const int x, const int y) const
{
 return ter_at(x, y).has_flag(flag);
}

bool map::has_flag_furn(const std::string & flag, const int x, const int y) const
{
 return furn_at(x, y).has_flag(flag);
}

bool map::has_flag_ter_or_furn(const std::string & flag, const int x, const int y) const
{
    if (!INBOUNDS(x, y)) {
        return false;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);

    return ( terlist[ current_submap->get_ter( lx, ly ) ].has_flag(flag) || furnlist[ current_submap->get_furn(lx, ly) ].has_flag(flag) );
}

bool map::has_flag_ter_and_furn(const std::string & flag, const int x, const int y) const
{
 return ter_at(x, y).has_flag(flag) && furn_at(x, y).has_flag(flag);
}
/////
bool map::has_flag(const ter_bitflags flag, const int x, const int y) const
{
    if (!INBOUNDS(x, y)) {
        return false;
    }

    int vpart;
    const vehicle *veh = veh_at( x, y, vpart );
    if( veh != nullptr && flag == TFLAG_REDUCE_SCENT && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    return has_flag_ter_or_furn(flag, x, y);
}

bool map::has_flag_ter(const ter_bitflags flag, const int x, const int y) const
{
 return ter_at(x, y).has_flag(flag);
}

bool map::has_flag_furn(const ter_bitflags flag, const int x, const int y) const
{
 return furn_at(x, y).has_flag(flag);
}

bool map::has_flag_ter_or_furn(const ter_bitflags flag, const int x, const int y) const
{
    if (!INBOUNDS(x, y)) {
        return false;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at(x, y, lx, ly);

    return ( terlist[ current_submap->get_ter( lx, ly ) ].has_flag(flag) || furnlist[ current_submap->get_furn(lx, ly) ].has_flag(flag) );
}

bool map::has_flag_ter_and_furn(const ter_bitflags flag, const int x, const int y) const
{
    if (!INBOUNDS(x, y)) {
        return false;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( x, y, lx, ly );

    return terlist[ current_submap->get_ter( lx, ly ) ].has_flag(flag) && furnlist[ current_submap->get_furn(lx, ly) ].has_flag(flag);
}

// End of 2D flags

bool map::has_flag( const std::string &flag, const tripoint &p ) const
{
    static const std::string flag_str_REDUCE_SCENT( "REDUCE_SCENT" ); // construct once per runtime, slash delay 90%
    if( !inbounds( p ) ) {
        return false;
    }

    int vpart;
    const vehicle *veh = veh_at( p, vpart );
    if( veh != nullptr && flag_str_REDUCE_SCENT == flag && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    return has_flag_ter_or_furn( flag, p );
}

bool map::can_put_items( const tripoint &p )
{
    return !has_flag( "NOITEM", p ) && !has_flag( "SEALED", p );
}

bool map::has_flag_ter( const std::string & flag, const tripoint &p ) const
{
    return ter_at( p ).has_flag( flag );
}

bool map::has_flag_furn( const std::string & flag, const tripoint &p ) const
{
    return furn_at( p ).has_flag( flag );
}

bool map::has_flag_ter_or_furn( const std::string & flag, const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return false;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return terlist[ current_submap->get_ter( lx, ly ) ].has_flag( flag ) ||
           furnlist[ current_submap->get_furn( lx, ly ) ].has_flag( flag );
}

bool map::has_flag_ter_and_furn( const std::string & flag, const tripoint &p ) const
{
    return ter_at( p ).has_flag( flag ) && furn_at( p ).has_flag( flag );
}

bool map::has_flag( const ter_bitflags flag, const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return false;
    }

    int vpart;
    const vehicle *veh = veh_at( p, vpart );
    if( veh != nullptr && flag == TFLAG_REDUCE_SCENT && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    return has_flag_ter_or_furn( flag, p );
}

bool map::has_flag_ter( const ter_bitflags flag, const tripoint &p ) const
{
    return ter_at( p ).has_flag( flag );
}

bool map::has_flag_furn( const ter_bitflags flag, const tripoint &p ) const
{
    return furn_at( p ).has_flag( flag );
}

bool map::has_flag_ter_or_furn( const ter_bitflags flag, const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return false;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return terlist[ current_submap->get_ter( lx, ly ) ].has_flag( flag ) ||
           furnlist[ current_submap->get_furn( lx, ly ) ].has_flag( flag );
}

bool map::has_flag_ter_and_furn( const ter_bitflags flag, const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return false;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return terlist[ current_submap->get_ter( lx, ly ) ].has_flag( flag ) &&
           furnlist[ current_submap->get_furn( lx, ly ) ].has_flag( flag );
}

// End of 3D flags

// Bashable - common function

int map::bash_rating_internal( const int str, const furn_t &furniture, 
                               const ter_t &terrain, const vehicle *veh, const int part ) const
{
    bool furn_smash = false;
    bool ter_smash = false;
    if( furniture.loadid != f_null && furniture.bash.str_max != -1 ) {
        furn_smash = true;
    } else if( terrain.bash.str_max != -1 ) {
        ter_smash = true;
    }

    if( veh != nullptr && veh->obstacle_at_part( part ) >= 0 ) {
        // Monsters only care about rating > 0, NPCs should want to path around cars instead
        return 2; // Should probably be a function of part hp (+armor on tile)
    }

    int bash_min = 0;
    int bash_max = 0;
    if( furn_smash ) {
        bash_min = furniture.bash.str_min;
        bash_max = furniture.bash.str_max;
    } else if( ter_smash ) {
        bash_min = terrain.bash.str_min;
        bash_max = terrain.bash.str_max;
    } else {
        return -1;
    }

    if (str < bash_min) {
        return 0;
    } else if (str >= bash_max) {
        return 10;
    }

    int ret = (10 * (str - bash_min)) / (bash_max - bash_min);
    // Round up to 1, so that desperate NPCs can try to bash down walls
    return std::max( ret, 1 );
}

// 2D bashable

bool map::is_bashable(const int x, const int y) const
{
    if( !inbounds(x, y) ) {
        DebugLog( D_WARNING, D_MAP ) << "Looking for out-of-bounds is_bashable at "
                                     << x << ", " << y;
        return false;
    }

    int vpart = -1;
    const vehicle *veh = veh_at( x, y, vpart );
    if( veh != nullptr && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    if( has_furn(x, y) && furn_at(x, y).bash.str_max != -1 ) {
        return true;
    } else if( ter_at(x, y).bash.str_max != -1 ) {
        return true;
    }

    return false;
}

bool map::is_bashable_ter(const int x, const int y) const
{
    if ( ter_at(x, y).bash.str_max != -1 ) {
        return true;
    }
    return false;
}

bool map::is_bashable_furn(const int x, const int y) const
{
    if ( has_furn(x, y) && furn_at(x, y).bash.str_max != -1 ) {
        return true;
    }
    return false;
}

bool map::is_bashable_ter_furn(const int x, const int y) const
{
    return is_bashable_furn(x, y) || is_bashable_ter(x, y);
}

int map::bash_strength(const int x, const int y) const
{
    if ( has_furn(x, y) && furn_at(x, y).bash.str_max != -1 ) {
        return furn_at(x, y).bash.str_max;
    } else if ( ter_at(x, y).bash.str_max != -1 ) {
        return ter_at(x, y).bash.str_max;
    }
    return -1;
}

int map::bash_resistance(const int x, const int y) const
{
    if ( has_furn(x, y) && furn_at(x, y).bash.str_min != -1 ) {
        return furn_at(x, y).bash.str_min;
    } else if ( ter_at(x, y).bash.str_min != -1 ) {
        return ter_at(x, y).bash.str_min;
    }
    return -1;
}

int map::bash_rating(const int str, const int x, const int y) const
{
    if (!inbounds(x, y)) {
        DebugLog( D_WARNING, D_MAP ) << "Looking for out-of-bounds is_bashable at "
                                     << x << ", " << y;
        return -1;
    }

    int part = -1;
    const furn_t &furniture = furn_at( x, y );
    const ter_t &terrain = ter_at( x, y );
    const vehicle *veh = veh_at( x, y, part );
    return bash_rating_internal( str, furniture, terrain, veh, part );
}

// 3D bashable

bool map::is_bashable( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        DebugLog( D_WARNING, D_MAP ) << "Looking for out-of-bounds is_bashable at "
                                     << p.x << ", " << p.y << ", " << p.z;
        return false;
    }

    int vpart = -1;
    const vehicle *veh = veh_at( p , vpart );
    if( veh != nullptr && veh->obstacle_at_part( vpart ) >= 0 ) {
        return true;
    }

    if( has_furn( p ) && furn_at( p ).bash.str_max != -1 ) {
        return true;
    } else if( ter_at( p ).bash.str_max != -1 ) {
        return true;
    }

    return false;
}

bool map::is_bashable_ter( const tripoint &p ) const
{
    if ( ter_at( p ).bash.str_max != -1 ) {
        return true;
    }
    return false;
}

bool map::is_bashable_furn( const tripoint &p ) const
{
    if ( has_furn( p ) && furn_at( p ).bash.str_max != -1 ) {
        return true;
    }
    return false;
}

bool map::is_bashable_ter_furn( const tripoint &p ) const
{
    return is_bashable_furn( p ) || is_bashable_ter( p );
}

int map::bash_strength( const tripoint &p ) const
{
    if ( has_furn( p ) && furn_at( p ).bash.str_max != -1 ) {
        return furn_at( p ).bash.str_max;
    } else if ( ter_at( p ).bash.str_max != -1 ) {
        return ter_at( p ).bash.str_max;
    }
    return -1;
}

int map::bash_resistance( const tripoint &p ) const
{
    if ( has_furn( p ) && furn_at( p ).bash.str_min != -1 ) {
        return furn_at( p ).bash.str_min;
    } else if ( ter_at( p ).bash.str_min != -1 ) {
        return ter_at( p ).bash.str_min;
    }
    return -1;
}

int map::bash_rating( const int str, const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        DebugLog( D_WARNING, D_MAP ) << "Looking for out-of-bounds is_bashable at "
                                     << p.x << ", " << p.y << ", " << p.z;
        return -1;
    }

    int part = -1;
    const furn_t &furniture = furn_at( p );
    const ter_t &terrain = ter_at( p );
    const vehicle *veh = veh_at( p, part );
    return bash_rating_internal( str, furniture, terrain, veh, part );
}

// End of 3D bashable

void map::make_rubble( const tripoint &p, furn_id rubble_type, bool items, ter_id floor_type, bool overwrite)
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    if (overwrite) {
        ter_set(x, y, floor_type);
        furn_set(x, y, rubble_type);
    } else {
        // First see if there is existing furniture to destroy
        if (is_bashable_furn(x, y)) {
            destroy_furn( p, true );
        }
        // Leave the terrain alone unless it interferes with furniture placement
        if (move_cost(x, y) <= 0 && is_bashable_ter(x, y)) {
            destroy( p, true );
        }
        // Check again for new terrain after potential destruction
        if (move_cost(x, y) <= 0) {
            ter_set(x, y, floor_type);
        }

        furn_set(x, y, rubble_type);
    }
    if (items) {
        //Still hardcoded, but a step up from the old stuff due to being in only one place
        if (rubble_type == f_wreckage) {
            item chunk("steel_chunk", calendar::turn);
            item scrap("scrap", calendar::turn);
            item pipe("pipe", calendar::turn);
            item wire("wire", calendar::turn);
            add_item_or_charges(x, y, chunk);
            add_item_or_charges(x, y, scrap);
            if (one_in(5)) {
                add_item_or_charges(x, y, pipe);
                add_item_or_charges(x, y, wire);
            }
        } else if (rubble_type == f_rubble_rock) {
            item rock("rock", calendar::turn);
            int rock_count = rng(1, 3);
            for (int i = 0; i < rock_count; i++) {
                add_item_or_charges(x, y, rock);
            }
        } else if (rubble_type == f_rubble) {
            item splinter("splinter", calendar::turn);
            item nail("nail", calendar::turn);
            int splinter_count = rng(2, 8);
            int nail_count = rng(5, 10);
            for (int i = 0; i < splinter_count; i++) {
                add_item_or_charges(x, y, splinter);
            }
            for (int i = 0; i < nail_count; i++) {
                add_item_or_charges(x, y, nail);
            }
        }
    }
}

/**
 * Returns whether or not the terrain at the given location can be dived into
 * (by monsters that can swim or are aquatic or nonbreathing).
 * @param x The x coordinate to look at.
 * @param y The y coordinate to look at.
 * @return true if the terrain can be dived into; false if not.
 */
bool map::is_divable(const int x, const int y) const
{
  return has_flag("SWIMMABLE", x, y) && has_flag(TFLAG_DEEP_WATER, x, y);
}

bool map::is_divable( const tripoint &p ) const
{
    return has_flag( "SWIMMABLE", p ) && has_flag( TFLAG_DEEP_WATER, p );
}

bool map::is_outside(const int x, const int y) const
{
 if(!INBOUNDS(x, y))
  return true;

 return outside_cache[x][y];
}

bool map::is_outside( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return true;
    }

    // TODO: Z
    return outside_cache[p.x][p.y];
}

bool map::is_last_ter_wall(const bool no_furn, const int x, const int y,
                           const int xmax, const int ymax, const direction dir) const {
    int xmov = 0;
    int ymov = 0;
    switch ( dir ) {
        case NORTH:
            ymov = -1;
            break;
        case SOUTH:
            ymov = 1;
            break;
        case WEST:
            xmov = -1;
            break;
        case EAST:
            xmov = 1;
            break;
        default:
            break;
    }
    int x2 = x;
    int y2 = y;
    bool result = true;
    bool loop = true;
    while ( (loop) && ((dir == NORTH && y2 >= 0) ||
                       (dir == SOUTH && y2 < ymax) ||
                       (dir == WEST  && x2 >= 0) ||
                       (dir == EAST  && x2 < xmax)) ) {
        if ( no_furn && has_furn(x2, y2) ) {
            loop = false;
            result = false;
        } else if ( !has_flag_ter("FLAT", x2, y2) ) {
            loop = false;
            if ( !has_flag_ter("WALL", x2, y2) ) {
                result = false;
            }
        }
        x2 += xmov;
        y2 += ymov;
    }
    return result;
}

bool map::flammable_items_at( const tripoint &p )
{
    for( const auto &i : i_at(p) ) {
        if( i.flammable() ) {
            // Total fire resistance == 0
            return true;
        }
    }

    return false;
}

bool map::moppable_items_at( const tripoint &p )
{
    for (auto &i : i_at(p)) {
        if (i.made_of(LIQUID)) {
            return true;
        }
    }
    const field &fld = field_at(p);
    if(fld.findField(fd_blood) != 0 || fld.findField(fd_blood_veggy) != 0 ||
          fld.findField(fd_blood_insect) != 0 || fld.findField(fd_blood_invertebrate) != 0
          || fld.findField(fd_gibs_flesh) != 0 || fld.findField(fd_gibs_veggy) != 0 ||
          fld.findField(fd_gibs_insect) != 0 || fld.findField(fd_gibs_invertebrate) != 0
          || fld.findField(fd_bile) != 0 || fld.findField(fd_slime) != 0 ||
          fld.findField(fd_sludge) != 0) {
        return true;
    }
    int vpart;
    vehicle *veh = veh_at(p, vpart);
    if(veh != 0) {
        std::vector<int> parts_here = veh->parts_at_relative(veh->parts[vpart].mount.x, veh->parts[vpart].mount.y);
        for(auto &i : parts_here) {
            if(veh->parts[i].blood > 0) {
                return true;
            }
        }
    }
    return false;
}

void map::decay_fields_and_scent( const int amount )
{
    // Decay scent separately, so that later we can use field count to skip empty submaps
    for( int x = 0; x < my_MAPSIZE * SEEX; x++ ) {
        for( int y = 0; y < my_MAPSIZE * SEEY; y++ ) {
            if( g->scent( x, y ) > 0 ) {
                g->scent( x, y )--;
            }
        }
    }

    const int amount_liquid = amount / 3; // Decay washable fields (blood, guts etc.) by this
    const int amount_gas = amount / 5; // Decay gas type fields by this
    // Coord code copied from lightmap calculations
    for( int smx = 0; smx < my_MAPSIZE; ++smx ) {
        for( int smy = 0; smy < my_MAPSIZE; ++smy ) {
            auto const cur_submap = get_submap_at_grid( smx, smy );
            int to_proc = cur_submap->field_count;
            if( to_proc < 1 ) {
                // This submap has no fields
                continue;
            }

            for( int sx = 0; sx < SEEX; ++sx ) {
                for( int sy = 0; sy < SEEY; ++sy ) {
                    const int x = sx + smx * SEEX;
                    const int y = sy + smy * SEEY;

                    if( !outside_cache[x][y] ) {
                        continue;
                    }

                    field &fields = cur_submap->fld[sx][sy];
                    for( auto &fp : fields ) {
                        to_proc--;
                        field_entry &cur = fp.second;
                        const field_id type = cur.getFieldType();
                        switch( type ) {
                            case fd_fire:
                                cur.setFieldAge( cur.getFieldAge() + amount );
                                break;
                            case fd_blood:
                            case fd_bile:
                            case fd_gibs_flesh:
                            case fd_gibs_veggy:
                            case fd_slime:
                            case fd_blood_veggy:
                            case fd_blood_insect:
                            case fd_blood_invertebrate:
                            case fd_gibs_insect:
                            case fd_gibs_invertebrate:
                                cur.setFieldAge( cur.getFieldAge() + amount_liquid );
                                break;
                            case fd_smoke:
                            case fd_toxic_gas:
                            case fd_tear_gas:
                            case fd_nuke_gas:
                            case fd_cigsmoke:
                            case fd_weedsmoke:
                            case fd_cracksmoke:
                            case fd_methsmoke:
                            case fd_relax_gas:
                            case fd_fungal_haze:
                            case fd_hot_air1:
                            case fd_hot_air2:
                            case fd_hot_air3:
                            case fd_hot_air4:
                                cur.setFieldAge( cur.getFieldAge() + amount_gas );
                                break;
                            default:
                                break;
                        }
                    }
                }
            }
        }
    }
}

point map::random_outdoor_tile()
{
 std::vector<point> options;
 for (int x = 0; x < SEEX * my_MAPSIZE; x++) {
  for (int y = 0; y < SEEY * my_MAPSIZE; y++) {
   if (is_outside(x, y))
    options.push_back(point(x, y));
  }
 }
 if (options.empty()) // Nowhere is outdoors!
  return point(-1, -1);

 return options[rng(0, options.size() - 1)];
}

bool map::has_adjacent_furniture( const tripoint &p )
{
    const signed char cx[4] = { 0, -1, 0, 1};
    const signed char cy[4] = {-1,  0, 1, 0};

    for (int i = 0; i < 4; i++)
    {
        const int adj_x = p.x + cx[i];
        const int adj_y = p.y + cy[i];
        if ( has_furn( tripoint( adj_x, adj_y, p.z ) ) && 
             furn_at( tripoint( adj_x, adj_y, p.z ) ).has_flag("BLOCKSDOOR") ) {
            return true;
        }
    }

 return false;
}

bool map::has_nearby_fire( const tripoint &p, int radius )
{
    for(int dx = -radius; dx <= radius; dx++) {
        for(int dy = -radius; dy <= radius; dy++) {
            const tripoint pt( p.x + dx, p.y + dy, p.z );
            if( get_field( pt, fd_fire ) != nullptr ) {
                return true;
            }
            if (ter(pt) == t_lava) {
                return true;
            }
        }
    }
    return false;
}

void map::mop_spills( const tripoint &p ) {
    auto items = i_at( p );
    for( auto it = items.begin(); it != items.end(); ) {
        if( it->made_of(LIQUID) ) {
            it = items.erase( it );
        } else {
            it++;
        }
    }
    remove_field( p, fd_blood );
    remove_field( p, fd_blood_veggy );
    remove_field( p, fd_blood_insect );
    remove_field( p, fd_blood_invertebrate );
    remove_field( p, fd_gibs_flesh );
    remove_field( p, fd_gibs_veggy );
    remove_field( p, fd_gibs_insect );
    remove_field( p, fd_gibs_invertebrate );
    remove_field( p, fd_bile );
    remove_field( p, fd_slime );
    remove_field( p, fd_sludge );
    int vpart;
    vehicle *veh = veh_at(p, vpart);
    if(veh != 0) {
        std::vector<int> parts_here = veh->parts_at_relative( veh->parts[vpart].mount.x,
                                                              veh->parts[vpart].mount.y );
        for( auto &elem : parts_here ) {
            veh->parts[elem].blood = 0;
        }
    }
}

void map::create_spores( const tripoint &p, Creature* source )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    // TODO: Infect NPCs?
    monster spore(GetMType("mon_spore"));
    int mondex;
    for (int i = x - 1; i <= x + 1; i++) {
        for (int j = y - 1; j <= y + 1; j++) {
            mondex = g->mon_at(i, j);
            if (move_cost(i, j) > 0 || (i == x && j == y)) {
                if (mondex != -1) { // Spores hit a monster
                    if (g->u.sees(i, j) &&
                        !g->zombie(mondex).type->in_species("FUNGUS")) {
                        add_msg(_("The %s is covered in tiny spores!"),
                                g->zombie(mondex).name().c_str());
                    }
                    monster &critter = g->zombie( mondex );
                    if( !critter.make_fungus() ) {
                        critter.die( source ); // counts as kill by player
                    }
                } else if (g->u.posx() == i && g->u.posy() == j) {
                    // Spores hit the player
                    bool hit = false;
                    if (one_in(4) && g->u.add_env_effect("spores", bp_head, 3, 90, bp_head)) {
                        hit = true;
                    }
                    if (one_in(2) && g->u.add_env_effect("spores", bp_torso, 3, 90, bp_torso)) {
                        hit = true;
                    }
                    if (one_in(4) && g->u.add_env_effect("spores", bp_arm_l, 3, 90, bp_arm_l)) {
                        hit = true;
                    }
                    if (one_in(4) && g->u.add_env_effect("spores", bp_arm_r, 3, 90, bp_arm_r)) {
                        hit = true;
                    }
                    if (one_in(4) && g->u.add_env_effect("spores", bp_leg_l, 3, 90, bp_leg_l)) {
                        hit = true;
                    }
                    if (one_in(4) && g->u.add_env_effect("spores", bp_leg_r, 3, 90, bp_leg_r)) {
                        hit = true;
                    }
                    if (hit) {
                        add_msg(m_warning, _("You're covered in tiny spores!"));
                    }
                } else if (((i == x && j == y) || one_in(4)) &&
                           g->num_zombies() <= 1000) { // Spawn a spore
                    spore.spawn(i, j);
                    g->add_zombie(spore);
                }
            }
        }
    }
}

int map::collapse_check( const tripoint &p )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    int num_supports = 0;
    tripoint t( p );
    int &i = t.x;
    int &j = t.y;
    for( i = x - 1; i <= x + 1; i++ ) {
        for( j = y - 1; j <= y + 1; j++ ) {
            if( p == t ) {
                continue;
            }
            if( has_flag( "COLLAPSES", p ) ) {
                if( has_flag( "COLLAPSES", t ) ) {
                    num_supports++;
                } else if( has_flag( "SUPPORTS_ROOF", t ) ) {
                    num_supports += 2;
                }
            } else if( has_flag( "SUPPORTS_ROOF", p ) ) {
                if( has_flag( "SUPPORTS_ROOF", t ) && !has_flag( "COLLAPSES", t ) ) {
                    num_supports += 3;
                }
            }
        }
    }
    return 1.7 * num_supports;
}

void map::collapse_at( const tripoint &p )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    destroy ( p, false );
    crush( p );
    make_rubble( p );
    tripoint t( p );
    int &i = t.x;
    int &j = t.y;
    for( i = x - 1; i <= x + 1; i++ ) {
        for( j = y - 1; j <= y + 1; j++ ) {
            if( p == t ) {
                continue;
            }
            if( has_flag( "COLLAPSES", t ) && one_in( collapse_check( t ) ) ) {
                destroy( t, false );
            // We only check for rubble spread if it doesn't already collapse to prevent double crushing
            } else if( has_flag("FLAT", t ) && one_in( 8 ) ) {
                crush( t );
                make_rubble( t );
            }
        }
    }
}

std::pair<bool, bool> map::bash( const tripoint &p, const int str,
                                 bool silent, bool destroy, vehicle *bashing_vehicle )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    bool success = false;
    int sound_volume = 0;
    std::string sound;
    bool smashed_something = false;
    if( get_field( point( x, y ), fd_web ) != nullptr ) {
        smashed_something = true;
        remove_field(x, y, fd_web);
    }

    // Destroy glass items, spilling their contents.
    std::vector<item> smashed_contents;
    auto bashed_items = i_at(x, y);
    for( auto bashed_item = bashed_items.begin(); bashed_item != bashed_items.end(); ) {
        // the check for active supresses molotovs smashing themselves with their own explosion
        if (bashed_item->made_of("glass") && !bashed_item->active && one_in(2)) {
            sound = _("glass shattering");
            sound_volume = 12;
            smashed_something = true;
            for( auto bashed_content : bashed_item->contents ) {
                smashed_contents.push_back( bashed_content );
            }
            bashed_item = bashed_items.erase( bashed_item );
        } else {
            ++bashed_item;
        }
    }
    // Now plunk in the contents of the smashed items.
    spawn_items( x, y, smashed_contents );

    // Smash vehicle if present
    int vpart;
    vehicle *veh = veh_at(x, y, vpart);
    if (veh && veh != bashing_vehicle) {
        veh->damage (vpart, str, 1);
        sound = _("crash!");
        sound_volume = 18;
        smashed_something = true;
        success = true;
    } else {
        // Else smash furniture or terrain
        bool smash_furn = false;
        bool smash_ter = false;
        map_bash_info *bash = NULL;

        if ( has_furn(x, y) && furn_at(x, y).bash.str_max != -1 ) {
            bash = &(furn_at(x,y).bash);
            smash_furn = true;
        } else if ( ter_at(x, y).bash.str_max != -1 ) {
            bash = &(ter_at(x,y).bash);
            smash_ter = true;
        }
        // TODO: what if silent is true?
        if (has_flag("ALARMED", x, y) && !g->event_queued(EVENT_WANTED)) {
            sounds::sound(x, y, 40, _("an alarm go off!"));
            // if the player is nearby blame him/her
            if( rl_dist( g->u.posx(), g->u.posy(), x, y ) <= 3 ) {
                g->u.add_memorial_log(pgettext("memorial_male", "Set off an alarm."),
                                      pgettext("memorial_female", "Set off an alarm."));
                const point abs = overmapbuffer::ms_to_sm_copy( getabs( x, y ) );
                g->add_event(EVENT_WANTED, int(calendar::turn) + 300, 0, tripoint( abs.x, abs.y, abs_sub.z ) );
            }
        }

        if ( bash != NULL && (!bash->destroy_only || destroy)) {
            int smin = bash->str_min;
            int smax = bash->str_max;
            int sound_vol = bash->sound_vol;
            int sound_fail_vol = bash->sound_fail_vol;
            if (destroy) {
                success = true;
            } else {
                if ( bash->str_min_blocked != -1 || bash->str_max_blocked != -1 ) {
                    if( has_adjacent_furniture( p ) ) {
                        if ( bash->str_min_blocked != -1 ) {
                            smin = bash->str_min_blocked;
                        }
                        if ( bash->str_max_blocked != -1 ) {
                            smax = bash->str_max_blocked;
                        }
                    }
                }
                if ( str >= smin && str >= rng(bash->str_min_roll, bash->str_max_roll)) {
                    success = true;
                }
            }

            if (success || destroy) {
                // Clear out any partially grown seeds
                if (has_flag_ter_or_furn("PLANT", x, y)) {
                    i_clear( x, y );
                }

                if (smash_furn) {
                    if (has_flag_furn("FUNGUS", p)) {
                        create_spores( p );
                    }
                } else if (smash_ter) {
                    if (has_flag_ter("FUNGUS", p)) {
                        create_spores( p );
                    }
                }

                if (destroy) {
                    sound_volume = smax;
                } else {
                    if (sound_vol == -1) {
                        sound_volume = std::min(int(smin * 1.5), smax);
                    } else {
                        sound_volume = sound_vol;
                    }
                }
                sound = _(bash->sound.c_str());
                // Set this now in case the ter_set below changes this
                bool collapses = has_flag("COLLAPSES", x, y) && smash_ter;
                bool supports = has_flag("SUPPORTS_ROOF", x, y) && smash_ter;
                if (smash_furn == true) {
                    furn_set(x, y, bash->furn_set);
                    // Hack alert.
                    // Signs have cosmetics associated with them on the submap since
                    // furniture can't store dynamic data to disk. To prevent writing
                    // mysteriously appearing for a sign later built here, remove the
                    // writing from the submap.
                    delete_signage( p );
                } else if (smash_ter == true) {
                    ter_set(x, y, bash->ter_set);
                } else {
                    debugmsg( "data/json/terrain.json does not have %s.bash.ter_set set!",
                              ter_at(x,y).id.c_str() );
                }

                spawn_item_list( bash->items, p );
                if (bash->explosive > 0) {
                    g->explosion( tripoint( x, y, abs_sub.z ), bash->explosive, 0, false);
                }

                if (collapses) {
                    collapse_at( p );
                }
                // Check the flag again to ensure the new terrain doesn't support anything
                if (supports && !has_flag( "SUPPORTS_ROOF", p) ) {
                    tripoint t( p );
                    int &i = t.x;
                    int &j = t.y;
                    for( i = x - 1; i <= x + 1; i++ ) {
                        for( j = y - 1; j <= y + 1; j++ ) {
                            if( p == t || !has_flag("COLLAPSES", t) ) {
                                continue;
                            }
                            if( one_in( collapse_check( t ) ) ) {
                                collapse_at( t );
                            }
                        }
                    }
                }
                smashed_something = true;
            } else {
                if (sound_fail_vol == -1) {
                    sound_volume = 12;
                } else {
                    sound_volume = sound_fail_vol;
                }
                sound = _(bash->sound_fail.c_str());
                smashed_something = true;
            }
        } else {
            furn_id furnid = furn(x, y);
            if ( furnid == f_skin_wall || furnid == f_skin_door || furnid == f_skin_door_o ||
                 furnid == f_skin_groundsheet || furnid == f_canvas_wall || furnid == f_canvas_door ||
                 furnid == f_canvas_door_o || furnid == f_groundsheet || furnid == f_fema_groundsheet) {
                if (str >= rng(0, 6) || destroy) {
                    // Special code to collapse the tent if destroyed
                    int tentx = -1, tenty = -1;
                    // Find the center of the tent
                    for (int i = -1; i <= 1; i++) {
                        for (int j = -1; j <= 1; j++) {
                            if (furn(x + i, y + j) == f_groundsheet ||
                                furn(x + i, y + j) == f_fema_groundsheet ||
                                furn(x + i, y + j) == f_skin_groundsheet){
                                tentx = x + i;
                                tenty = y + j;
                                break;
                            }
                        }
                    }
                    // Never found tent center, bail out
                    if (tentx == -1 && tenty == -1) {
                        smashed_something = true;
                    }
                    // Take the tent down
                    for (int i = -1; i <= 1; i++) {
                        for (int j = -1; j <= 1; j++) {
                            if (furn(tentx + i, tenty + j) == f_groundsheet) {
                                spawn_item(tentx + i, tenty + j, "broketent");
                            }
                            if (furn(tentx + i, tenty + j) == f_skin_groundsheet) {
                                spawn_item(tentx + i, tenty + j, "damaged_shelter_kit");
                            }
                            furn_id check_furn = furn(tentx + i, tenty + j);
                            if (check_furn == f_skin_wall || check_furn == f_skin_door ||
                                  check_furn == f_skin_door_o || check_furn == f_skin_groundsheet ||
                                  check_furn == f_canvas_wall || check_furn == f_canvas_door ||
                                  check_furn == f_canvas_door_o || check_furn == f_groundsheet ||
                                  check_furn == f_fema_groundsheet) {
                                furn_set(tentx + i, tenty + j, f_null);
                            }
                        }
                    }

                    sound_volume = 8;
                    sound = _("rrrrip!");
                    smashed_something = true;
                    success = true;
                } else {
                    sound_volume = 8;
                    sound = _("slap!");
                    smashed_something = true;
                }
            // Made furniture seperate from the other tent to facilitate destruction
            } else if (furnid == f_center_groundsheet || furnid == f_large_groundsheet ||
                     furnid == f_large_canvas_door || furnid == f_large_canvas_wall ||
                     furnid == f_large_canvas_door_o) {
                if (str >= rng(0, 6) || destroy) {
                    // Special code to collapse the tent if destroyed
                    int tentx = -1, tenty = -1;
                    // Find the center of the tent
                    for (int i = -2; i <= 2; i++) {
                        for (int j = -2; j <= 2; j++) {
                            if (furn(x + i, y + j) == f_center_groundsheet){
                                tentx = x + i;
                                tenty = y + j;
                                break;
                            }
                        }
                    }
                    // Never found tent center, bail out
                    if (tentx == -1 && tenty == -1) {
                        smashed_something = true;
                    }
                    // Take the tent down
                    for (int i = -2; i <= 2; i++) {
                        for (int j = -2; j <= 2; j++) {
                             if (furn(tentx + i, tenty + j) == f_center_groundsheet) {
                             spawn_item(tentx + i, tenty + j, "largebroketent");
                            }
                            furn_set(tentx + i, tenty + j, f_null);
                        }
                    }
                    sound_volume = 8;
                    sound = _("rrrrip!");
                    smashed_something = true;
                    success = true;
                } else {
                    sound_volume = 8;
                    sound = _("slap!");
                    smashed_something = true;
                }
            }
        }
    }
    if( move_cost(x, y) <= 0  && !smashed_something ) {
        sound = _("thump!");
        sound_volume = 18;
        smashed_something = true;
    }
    if( !sound.empty() && !silent) {
        sounds::sound( x, y, sound_volume, sound);
    }
    return std::pair<bool, bool> (smashed_something, success);
}

void map::spawn_item_list( const std::vector<map_bash_item_drop> &items, const tripoint &p ) {
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    for( auto &items_i : items ) {
        const map_bash_item_drop &drop = items_i;
        int chance = drop.chance;
        if ( chance == -1 || rng(0, 100) >= chance ) {
            int numitems = drop.amount;

            if ( drop.minamount != -1 ) {
                numitems = rng( drop.minamount, drop.amount );
            }
            if ( numitems > 0 ) {
                // spawn_item(x,y, drop.itemtype, numitems); // doesn't abstract amount || charges
                item new_item(drop.itemtype, calendar::turn);
                if ( new_item.count_by_charges() ) {
                    new_item.charges = numitems;
                    numitems = 1;
                }
                const bool varsize = new_item.has_flag( "VARSIZE" );
                for(int a = 0; a < numitems; a++ ) {
                    if( varsize && one_in( 3 ) ) {
                        new_item.item_tags.insert( "FIT" );
                    } else if( varsize ) {
                        // might have been added previously
                        new_item.item_tags.erase( "FIT" );
                    }
                    add_item_or_charges(x, y, new_item);
                }
            }
        }
    }
}

void map::destroy( const tripoint &p, const bool silent )
{
    // Break if it takes more than 25 destructions to remove to prevent infinite loops
    // Example: A bashes to B, B bashes to A leads to A->B->A->...
    int count = 0;
    // TODO: Z
    while( count <= 25 && bash( p, 999, silent, true ).second ) {
        count++;
    }
}

void map::destroy_furn( const tripoint &p, const bool silent )
{
    // Break if it takes more than 25 destructions to remove to prevent infinite loops
    // Example: A bashes to B, B bashes to A leads to A->B->A->...
    int count = 0;
    while (count <= 25 && furn(p) != f_null && bash(p, 999, silent, true).second) {
        count++;
    }
}

void map::crush( const tripoint &p )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    int veh_part;
    player *crushed_player = nullptr;
    //The index of the NPC at (x,y), or -1 if there isn't one
    int npc_index = g->npc_at(x, y);
    if( g->u.posx() == x && g->u.posy() == y ) {
        crushed_player = &(g->u);
    } else if( npc_index != -1 ) {
        crushed_player = static_cast<player *>(g->active_npc[npc_index]);
    }

    if( crushed_player != nullptr ) {
        bool player_inside = false;
        if( crushed_player->in_vehicle ) {
            vehicle *veh = veh_at(x, y, veh_part);
            player_inside = (veh && veh->is_inside(veh_part));
        }
        if (!player_inside) { //If there's a player at (x,y) and he's not in a covered vehicle...
            //This is the roof coming down on top of us, no chance to dodge
            crushed_player->add_msg_player_or_npc( m_bad, _("You are crushed by the falling debris!"),
                                                   _("<npcname> is crushed by the falling debris!") );
            int dam = rng(0, 40);
            // Torso and head take the brunt of the blow
            body_part hit = bp_head;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .25 ) );
            hit = bp_torso;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .45 ) );
            // Legs take the next most through transfered force
            hit = bp_leg_l;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .10 ) );
            hit = bp_leg_r;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .10 ) );
            // Arms take the least
            hit = bp_arm_l;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .05 ) );
            hit = bp_arm_r;
            crushed_player->deal_damage( nullptr, hit, damage_instance( DT_BASH, dam * .05 ) );

            // Pin whoever got hit
            crushed_player->add_effect("crushed", 1, num_bp, true);
            crushed_player->check_dead_state();
        }
    }

    //The index of the monster at (x,y), or -1 if there isn't one
    int mon = g->mon_at(x, y);
    if (mon != -1 && size_t(mon) < g->num_zombies()) {  //If there's a monster at (x,y)...
        monster* monhit = &(g->zombie(mon));
        // 25 ~= 60 * .45 (torso)
        monhit->deal_damage(nullptr, bp_torso, damage_instance(DT_BASH, rng(0,25)));

        // Pin whoever got hit
        monhit->add_effect("crushed", 1, num_bp, true);
        monhit->check_dead_state();
    }

    vehicle *veh = veh_at(x, y, veh_part);
    if (veh) {
        veh->damage(veh_part, rng(0, veh->parts[veh_part].hp), 1, false);
    }
}

void map::shoot( const tripoint &p, int &dam,
                 const bool hit_items, const std::set<std::string>& ammo_effects )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    if (dam < 0)
    {
        return;
    }

    if (has_flag("ALARMED", x, y) && !g->event_queued(EVENT_WANTED))
    {
        sounds::sound(x, y, 30, _("An alarm sounds!"));
        const point abs = overmapbuffer::ms_to_sm_copy( getabs( x, y ) );
        g->add_event(EVENT_WANTED, int(calendar::turn) + 300, 0, tripoint( abs.x, abs.y, abs_sub.z ) );
    }

    int vpart;
    vehicle *veh = veh_at(x, y, vpart);
    if (veh)
    {
        const bool inc = (ammo_effects.count("INCENDIARY") || ammo_effects.count("FLAME"));
        dam = veh->damage (vpart, dam, inc? 2 : 0, hit_items);
    }

    ter_id terrain = ter( p );
    if( terrain == t_wall_wood_broken ||
        terrain == t_wall_log_broken ||
        terrain == t_door_b ) {
        if (hit_items || one_in(8)) { // 1 in 8 chance of hitting the door
            dam -= rng(20, 40);
            if (dam > 0) {
                sounds::sound(x, y, 10, _("crash!"));
                ter_set(x, y, t_dirt);
            }
        }
        else {
            dam -= rng(0, 1);
        }
    } else if( terrain == t_door_c ||
               terrain == t_door_locked ||
               terrain == t_door_locked_peep ||
               terrain == t_door_locked_alarm ) {
        dam -= rng(15, 30);
        if (dam > 0) {
            sounds::sound(x, y, 10, _("smash!"));
            ter_set(x, y, t_door_b);
        }
    } else if( terrain == t_door_boarded ||
               terrain == t_door_boarded_damaged ||
               terrain == t_rdoor_boarded ||
               terrain == t_rdoor_boarded_damaged ) {
        dam -= rng(15, 35);
        if (dam > 0) {
            sounds::sound(x, y, 10, _("crash!"));
            ter_set(x, y, t_door_b);
        }
    } else if( terrain == t_window_domestic_taped ||
               terrain == t_curtains ) {
        if (ammo_effects.count("LASER")) {
            dam -= rng(1, 5);
        }
        if (ammo_effects.count("LASER")) {
            dam -= rng(0, 5);
        } else {
            dam -= rng(1,3);
            if (dam > 0) {
                sounds::sound(x, y, 16, _("glass breaking!"));
                ter_set(x, y, t_window_frame);
                spawn_item(x, y, "sheet", 1);
                spawn_item(x, y, "stick");
                spawn_item(x, y, "string_36");
            }
        }
    } else if( terrain == t_window_domestic ) {
        if (ammo_effects.count("LASER")) {
            dam -= rng(0, 5);
        } else {
            dam -= rng(1,3);
            if (dam > 0) {
                sounds::sound(x, y, 16, _("glass breaking!"));
                ter_set(x, y, t_window_frame);
                spawn_item(x, y, "sheet", 1);
                spawn_item(x, y, "stick");
                spawn_item(x, y, "string_36");
            }
        }
    } else if( terrain == t_window_taped ||
               terrain == t_window_alarm_taped ) {
        if (ammo_effects.count("LASER")) {
            dam -= rng(1, 5);
        }
        if (ammo_effects.count("LASER")) {
            dam -= rng(0, 5);
        } else {
            dam -= rng(1,3);
            if (dam > 0) {
                sounds::sound(x, y, 16, _("glass breaking!"));
                ter_set(x, y, t_window_frame);
            }
        }
    } else if( terrain == t_window ||
               terrain == t_window_alarm ) {
        if (ammo_effects.count("LASER")) {
            dam -= rng(0, 5);
        } else {
            dam -= rng(1,3);
            if (dam > 0) {
                sounds::sound(x, y, 16, _("glass breaking!"));
                ter_set(x, y, t_window_frame);
            }
        }
    } else if( terrain == t_window_boarded ) {
        dam -= rng(10, 30);
        if (dam > 0) {
            sounds::sound(x, y, 16, _("glass breaking!"));
            ter_set(x, y, t_window_frame);
        }
    } else if( terrain == t_wall_glass  ||
               terrain == t_wall_glass_alarm ||
               terrain == t_door_glass_c ) {
        if (ammo_effects.count("LASER")) {
            dam -= rng(0,5);
        } else {
            dam -= rng(1,8);
            if (dam > 0) {
                sounds::sound(x, y, 20, _("glass breaking!"));
                ter_set(x, y, t_floor);
            }
        }
    } else if( terrain == t_reinforced_glass ) {
        // reinforced glass stops most bullets
        // laser beams are attenuated
        if (ammo_effects.count("LASER")) {
            dam -= rng(0, 8);
        } else {
            //Greatly weakens power of bullets
            dam -= 40;
            if (dam <= 0) {
                add_msg(_("The shot is stopped by the reinforced glass wall!"));
            } else if (dam >= 40) {
                //high powered bullets penetrate the glass, but only extremely strong
                // ones (80 before reduction) actually destroy the glass itself.
                sounds::sound(x, y, 20, _("glass breaking!"));
                ter_set(x, y, t_floor);
            }
        }
    } else if( terrain == t_paper ) {
        dam -= rng(4, 16);
        if (dam > 0) {
            sounds::sound(x, y, 8, _("rrrrip!"));
            ter_set(x, y, t_dirt);
        }
        if (ammo_effects.count("INCENDIARY")) {
            add_field(x, y, fd_fire, 1);
        }
    } else if( terrain == t_gas_pump ) {
        if (hit_items || one_in(3)) {
            if (dam > 15) {
                if (ammo_effects.count("INCENDIARY") || ammo_effects.count("FLAME")) {
                    g->explosion( tripoint( x, y, abs_sub.z ), 40, 0, true);
                } else {
                    for (int i = x - 2; i <= x + 2; i++) {
                        for (int j = y - 2; j <= y + 2; j++) {
                            if (move_cost(i, j) > 0 && one_in(3)) {
                                    spawn_item(i, j, "gasoline");
                            }
                        }
                    }
                    sounds::sound(x, y, 10, _("smash!"));
                }
                ter_set(x, y, t_gas_pump_smashed);
            }
            dam -= 60;
        }
    } else if( terrain == t_vat ) {
        if (dam >= 10) {
            sounds::sound(x, y, 20, _("ke-rash!"));
            ter_set(x, y, t_floor);
        } else {
            dam = 0;
        }
    } else {
        if (move_cost(x, y) == 0 && !trans(x, y)) {
            dam = 0; // TODO: Bullets can go through some walls?
        } else {
            dam -= (rng(0, 1) * rng(0, 1) * rng(0, 1));
        }
    }

    if (ammo_effects.count("TRAIL") && !one_in(4)) {
        add_field(x, y, fd_smoke, rng(1, 2));
    }

    if (ammo_effects.count("STREAM") && !one_in(3)) {
        add_field(x, y, fd_fire, rng(1, 2));
    }

    if (ammo_effects.count("STREAM_BIG") && !one_in(4)) {
        add_field(x, y, fd_fire, 2);
    }

    if (ammo_effects.count("LIGHTNING")) {
        add_field(x, y, fd_electricity, rng(2, 3));
    }

    if (ammo_effects.count("PLASMA") && one_in(2)) {
        add_field(x, y, fd_plasma, rng(1, 2));
    }

    if (ammo_effects.count("LASER")) {
        add_field(x, y, fd_laser, 2);
    }

    // Set damage to 0 if it's less
    if (dam < 0) {
        dam = 0;
    }

    // Check fields?
    const field_entry *fieldhit = get_field( point( x, y ), fd_web );
    if( fieldhit != nullptr ) {
        if (ammo_effects.count("INCENDIARY") || ammo_effects.count("FLAME")) {
            add_field(x, y, fd_fire, fieldhit->getFieldDensity() - 1);
        } else if (dam > 5 + fieldhit->getFieldDensity() * 5 &&
                   one_in(5 - fieldhit->getFieldDensity())) {
            dam -= rng(1, 2 + fieldhit->getFieldDensity() * 2);
            remove_field(x, y,fd_web);
        }
    }

    // Now, destroy items on that tile.
    if ((move_cost(x, y) == 2 && !hit_items) || !INBOUNDS(x, y)) {
        return; // Items on floor-type spaces won't be shot up.
    }

    auto target_items = i_at(x, y);
    for( auto target_item = target_items.begin(); target_item != target_items.end(); ) {
        bool destroyed = false;
        int chance = ( target_item->volume() > 0 ? target_item->volume() : 1);
        // volume dependent chance

        if( dam > target_item->bash_resist() && one_in(chance) ) {
            target_item->damage++;
        }
        if( target_item->damage >= 5 ) {
            destroyed = true;
        }

        if (destroyed) {
            spawn_items( x, y, target_item->contents );
            target_item = target_items.erase( target_item );
        } else {
            ++target_item;
        }
    }
}

bool map::hit_with_acid( const tripoint &p )
{
    if( move_cost( p ) != 0 ) {
        return false;    // Didn't hit the tile!
    }
    const ter_id t = ter( p );
    if( t == t_wall_glass || t == t_wall_glass_alarm ||
        t == t_vat ) {
        ter_set( p, t_floor );
    } else if( t == t_door_c || t == t_door_locked || t == t_door_locked_peep || t == t_door_locked_alarm ) {
        if( one_in( 3 ) ) {
            ter_set( p, t_door_b );
        }
    } else if( t == t_door_bar_c || t == t_door_bar_o || t == t_door_bar_locked || t == t_bars ) {
        ter_set( p, t_floor );
        add_msg( m_warning, _( "The metal bars melt!" ) );
    } else if( t == t_door_b ) {
        if( one_in( 4 ) ) {
            ter_set( p, t_door_frame );
        } else {
            return false;
        }
    } else if( t == t_window || t == t_window_alarm ) {
        ter_set( p, t_window_empty );
    } else if( t == t_wax ) {
        ter_set( p, t_floor_wax );
    } else if( t == t_gas_pump || t == t_gas_pump_smashed ) {
        return false;
    } else if( t == t_card_science || t == t_card_military ) {
        ter_set( p, t_card_reader_broken );
    }
    return true;
}

// returns true if terrain stops fire
bool map::hit_with_fire( const tripoint &p )
{
    if (move_cost( p ) != 0)
        return false; // Didn't hit the tile!

    // non passable but flammable terrain, set it on fire
    if (has_flag("FLAMMABLE", p ) || has_flag("FLAMMABLE_ASH", p))
    {
        add_field(p, fd_fire, 3, 0);
    }
    return true;
}

bool map::marlossify( const tripoint &p )
{
    if (one_in(25) && (ter_at(p).movecost != 0 && !has_furn(p))
            && !ter_at(p).has_flag(TFLAG_DEEP_WATER)) {
        ter_set(p, t_marloss);
        return true;
    }
    for (int i = 0; i < 25; i++) {
        if(!g->spread_fungus( p )) {
            return true;
        }
    }
    return false;
}

bool map::open_door( const tripoint &p, const bool inside, const bool check_only )
{
    // TODO: Z
    const int x = p.x;
    const int y = p.y;
    const auto &ter = ter_at( x, y );
    const auto &furn = furn_at( x, y );
    int vpart = -1;
    vehicle *veh = veh_at( x, y, vpart );
    if ( !ter.open.empty() && ter.open != "t_null" ) {
        if ( termap.find( ter.open ) == termap.end() ) {
            debugmsg("terrain %s.open == non existant terrain '%s'\n", ter.id.c_str(), ter.open.c_str() );
            return false;
        }

        if ( has_flag("OPENCLOSE_INSIDE", x, y) && inside == false ) {
            return false;
        }

        if(!check_only) {
            ter_set(x, y, ter.open );
        }

        return true;
    } else if ( !furn.open.empty() && furn.open != "t_null" ) {
        if ( furnmap.find( furn.open ) == furnmap.end() ) {
            debugmsg("terrain %s.open == non existant furniture '%s'\n", furn.id.c_str(), furn.open.c_str() );
            return false;
        }

        if ( has_flag("OPENCLOSE_INSIDE", x, y) && inside == false ) {
            return false;
        }

        if(!check_only) {
            furn_set(x, y, furn.open );
        }

        return true;
    } else if( veh != nullptr ) {
        int openable = veh->next_part_to_open( vpart, true );
        if (openable >= 0) {
            if( !check_only ) {
                veh->open_all_at( openable );
            }

            return true;
        }

        return false;
    }

    return false;
}

void map::translate(const ter_id from, const ter_id to)
{
    if (from == to) {
        debugmsg( "map::translate %s => %s", 
                  terlist[from].name.c_str(),
                  terlist[from].name.c_str() );
        return;
        }

        tripoint p( 0, 0, abs_sub.z );
        int &x = p.x;
        int &y = p.y;
        for( x = 0; x < SEEX * my_MAPSIZE; x++ ) {
            for( y = 0; y < SEEY * my_MAPSIZE; y++ ) {
            if( ter( p ) == from ) {
                ter_set( p, to );
            }
        }
    }
}

//This function performs the translate function within a given radius of the player.
void map::translate_radius(const ter_id from, const ter_id to, float radi, const tripoint &p )
{
    if( from == to ) {
        debugmsg( "map::translate %s => %s", 
                  terlist[from].name.c_str(),
                  terlist[from].name.c_str() );
        return;
    }

    const int uX = p.x;
    const int uY = p.y;
    tripoint t( 0, 0, abs_sub.z );
    int &x = t.x;
    int &y = t.y;
    for( x = 0; x < SEEX * my_MAPSIZE; x++ ) {
        for( y = 0; y < SEEY * my_MAPSIZE; y++ ) {
            if( ter( t ) == from ) {
                float radiX = sqrt(float((uX-x)*(uX-x) + (uY-y)*(uY-y)));
                if( radiX <= radi ){
                    ter_set( t, to);
                }
            }
        }
    }
}

bool map::close_door( const tripoint &p, const bool inside, const bool check_only )
{
 const auto &ter = ter_at( p );
 const auto &furn = furn_at( p );
 if ( !ter.close.empty() && ter.close != "t_null" ) {
     if ( termap.find( ter.close ) == termap.end() ) {
         debugmsg("terrain %s.close == non existant terrain '%s'\n", ter.id.c_str(), ter.close.c_str() );
         return false;
     }
     if ( has_flag("OPENCLOSE_INSIDE", p) && inside == false ) {
         return false;
     }
     if (!check_only) {
        ter_set(p, ter.close );
     }
     return true;
 } else if ( !furn.close.empty() && furn.close != "t_null" ) {
     if ( furnmap.find( furn.close ) == furnmap.end() ) {
         debugmsg("terrain %s.close == non existant furniture '%s'\n", furn.id.c_str(), furn.close.c_str() );
         return false;
     }
     if ( has_flag("OPENCLOSE_INSIDE", p) && inside == false ) {
         return false;
     }
     if (!check_only) {
         furn_set(p, furn.close );
     }
     return true;
 }
 return false;
}

const std::string map::get_signage( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return "";
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    return current_submap->get_signage(lx, ly);
}
void map::set_signage( const tripoint &p, std::string message ) const
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    current_submap->set_signage(lx, ly, message);
}
void map::delete_signage( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    current_submap->delete_signage(lx, ly);
}

int map::get_radiation( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return 0;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->get_radiation( lx, ly );
}

void map::set_radiation( const int x, const int y, const int value )
{
    set_radiation( tripoint( x, y, abs_sub.z ), value );
}

void map::set_radiation( const tripoint &p, const int value)
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    current_submap->set_radiation( lx, ly, value );
}

void map::adjust_radiation( const int x, const int y, const int delta )
{
    adjust_radiation( tripoint( x, y, abs_sub.z ), delta );
}

void map::adjust_radiation( const tripoint &p, const int delta )
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    int current_radiation = current_submap->get_radiation( lx, ly );
    current_submap->set_radiation( lx, ly, current_radiation + delta );
}

int& map::temperature( const tripoint &p )
{
    if( !inbounds( p ) ) {
        null_temperature = 0;
        return null_temperature;
    }

    return get_submap_at( p )->temperature;
}

void map::set_temperature( const tripoint &p, int new_temperature )
{
    temperature( p ) = new_temperature;
    temperature( tripoint( p.x + SEEX, p.y, p.z ) ) = new_temperature;
    temperature( tripoint( p.x, p.y + SEEY, p.z ) ) = new_temperature;
    temperature( tripoint( p.x + SEEX, p.y + SEEY, p.z ) ) = new_temperature;
}

void map::set_temperature( const int x, const int y, int new_temperature )
{
    set_temperature( tripoint( x, y, abs_sub.z ), new_temperature );
}

// Items: 2D
map_stack map::i_at( const int x, const int y )
{
    if( !INBOUNDS(x, y) ) {
        nulitems.clear();
        return map_stack{ &nulitems, tripoint( x, y, abs_sub.z ), this };
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( x, y, lx, ly );

    return map_stack{ &current_submap->itm[lx][ly], tripoint( x, y, abs_sub.z ), this };
}

std::list<item>::iterator map::i_rem( const point location, std::list<item>::iterator it )
{
    return i_rem( tripoint( location, abs_sub.z ), it );
}

int map::i_rem(const int x, const int y, const int index)
{
    return i_rem( tripoint( x, y, abs_sub.z ), index );
}

void map::i_rem(const int x, const int y, item *it)
{
    i_rem( tripoint( x, y, abs_sub.z ), it );
}

void map::i_clear(const int x, const int y)
{
    i_clear( tripoint( x, y, abs_sub.z ) );
}

void map::spawn_an_item(const int x, const int y, item new_item,
                        const long charges, const int damlevel)
{
    spawn_an_item( tripoint( x, y, abs_sub.z ), new_item, charges, damlevel );
}

void map::spawn_items(const int x, const int y, const std::vector<item> &new_items)
{
    spawn_items( tripoint( x, y, abs_sub.z ), new_items );
}

void map::spawn_item(const int x, const int y, const std::string &type_id,
                     const unsigned quantity, const long charges,
                     const unsigned birthday, const int damlevel, const bool rand)
{
    spawn_item( tripoint( x, y, abs_sub.z ), type_id, 
                quantity, charges, birthday, damlevel, rand );
}

int map::max_volume(const int x, const int y)
{
    const ter_t &ter = ter_at(x, y);
    if (has_furn(x, y)) {
        return furn_at(x, y).max_volume;
    }
    return ter.max_volume;
}

int map::stored_volume(const int x, const int y)
{
    return stored_volume( tripoint( x, y, abs_sub.z ) );
}

int map::free_volume(const int x, const int y)
{
    return free_volume( tripoint( x, y, abs_sub.z ) );
}

bool map::add_item_or_charges(const int x, const int y, item new_item, int overflow_radius)
{
    return add_item_or_charges( tripoint( x, y, abs_sub.z ), new_item, overflow_radius );
}

void map::add_item(const int x, const int y, item new_item)
{
    add_item( tripoint( x, y, abs_sub.z ), new_item );
}

// Items: 3D

map_stack map::i_at( const tripoint &p )
{
    if( !inbounds(p) ) {
        nulitems.clear();
        return map_stack{ &nulitems, p, this };
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return map_stack{ &current_submap->itm[lx][ly], p, this };
}

std::list<item>::iterator map::i_rem( const tripoint &p, std::list<item>::iterator it )
{
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    if( current_submap->active_items.has( it, point( lx, ly ) ) ) {
        current_submap->active_items.remove( it, point( lx, ly ) );
    }

    current_submap->update_lum_rem(*it, lx, ly);

    return current_submap->itm[lx][ly].erase( it );
}

int map::i_rem(const tripoint &p, const int index)
{
    if (index > (int)i_at(p).size() - 1) {
        return index;
    }
    auto map_items = i_at(p);

    int i = 0;
    for( auto iter = map_items.begin(); iter != map_items.end(); iter++, i++ ) {
        if( i == index) {
            map_items.erase( iter );
            return i;
        }
    }
    return index;
}

void map::i_rem(const tripoint &p, item *it)
{
    auto map_items = i_at(p);

    for( auto iter = map_items.begin(); iter != map_items.end(); iter++ ) {
        //delete the item if the pointer memory addresses are the same
        if(it == &*iter) {
            map_items.erase(iter);
            break;
        }
    }
}

void map::i_clear(const tripoint &p)
{
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    for( auto item_it = current_submap->itm[lx][ly].begin();
         item_it != current_submap->itm[lx][ly].end(); ++item_it ) {
        if( current_submap->active_items.has( item_it, point( lx, ly ) ) ) {
            current_submap->active_items.remove( item_it, point( lx, ly ) );
        }
    }

    current_submap->lum[lx][ly] = 0;
    current_submap->itm[lx][ly].clear();
}

void map::spawn_an_item(const tripoint &p, item new_item,
                        const long charges, const int damlevel)
{
    if( charges && new_item.charges > 0 ) {
        //let's fail silently if we specify charges for an item that doesn't support it
        new_item.charges = charges;
    }
    new_item = new_item.in_its_container();
    if( (new_item.made_of(LIQUID) && has_flag("SWIMMABLE", p)) ||
        has_flag("DESTROY_ITEM", p) ) {
        return;
    }
    // bounds checking for damage level
    if( damlevel < -1 ) {
        new_item.damage = -1;
    } else if( damlevel > 4 ) {
        new_item.damage = 4;
    } else {
        new_item.damage = damlevel;
    }
    add_item_or_charges(p, new_item);
}

void map::spawn_items(const tripoint &p, const std::vector<item> &new_items)
{
    if (!inbounds(p) || has_flag("DESTROY_ITEM", p)) {
        return;
    }
    const bool swimmable = has_flag("SWIMMABLE", p);
    for( auto new_item : new_items ) {

        if (new_item.made_of(LIQUID) && swimmable) {
            continue;
        }
        if (new_item.is_armor() && new_item.has_flag("PAIRED") && x_in_y(4, 5)) {
            item new_item2 = new_item;
            new_item.make_handed( LEFT );
            new_item2.make_handed( RIGHT );
            add_item_or_charges(p, new_item2);
        }
        add_item_or_charges(p, new_item);
    }
}

void map::spawn_artifact(const tripoint &p)
{
    add_item_or_charges( p, item( new_artifact(), 0 ) );
}

void map::spawn_natural_artifact(const tripoint &p, artifact_natural_property prop)
{
    add_item_or_charges( p, item( new_natural_artifact( prop ), 0 ) );
}

//New spawn_item method, using item factory
// added argument to spawn at various damage levels
void map::spawn_item(const tripoint &p, const std::string &type_id,
                     const unsigned quantity, const long charges,
                     const unsigned birthday, const int damlevel, const bool rand)
{
    if(type_id == "null") {
        return;
    }
    if(item_is_blacklisted(type_id)) {
        return;
    }
    // recurse to spawn (quantity - 1) items
    for(unsigned i = 1; i < quantity; i++)
    {
        spawn_item(p, type_id, 1, charges, birthday, damlevel);
    }
    // spawn the item
    item new_item(type_id, birthday, rand);
    if( one_in( 3 ) && new_item.has_flag( "VARSIZE" ) ) {
        new_item.item_tags.insert( "FIT" );
    }
    spawn_an_item(p, new_item, charges, damlevel);
}

int map::max_volume(const tripoint &p)
{
    const ter_t &ter = ter_at(p);
    if (has_furn(p)) {
        return furn_at(p).max_volume;
    }
    return ter.max_volume;
}

// total volume of all the things
int map::stored_volume(const tripoint &p) {
    if(!inbounds(p)) {
        return 0;
    }
    int cur_volume = 0;
    for( auto &n : i_at(p) ) {
        cur_volume += n.volume();
    }
    return cur_volume;
}

// free space
int map::free_volume(const tripoint &p) {
   const int maxvolume = this->max_volume(p);
   if(!inbounds(p)) return 0;
   return ( maxvolume - stored_volume(p) );
}

// returns true if full, modified by arguments:
// (none):                            size >= max || volume >= max
// (addvolume >= 0):                  size+1 > max || volume + addvolume > max
// (addvolume >= 0, addnumber >= 0):  size + addnumber > max || volume + addvolume > max
bool map::is_full(const tripoint &p, const int addvolume, const int addnumber ) {
   const int maxitems = MAX_ITEM_IN_SQUARE; // (game.h) 1024
   const int maxvolume = this->max_volume(p);

   if( ! (inbounds(p) && move_cost(p) > 0 && !has_flag("NOITEM", p) ) ) {
       return true;
   }

   if ( addvolume == -1 ) {
       if ( (int)i_at(p).size() < maxitems ) return true;
       int cur_volume=stored_volume(p);
       return (cur_volume >= maxvolume ? true : false );
   } else {
       if ( (int)i_at(p).size() + ( addnumber == -1 ? 1 : addnumber ) > maxitems ) return true;
       int cur_volume=stored_volume(p);
       return ( cur_volume + addvolume > maxvolume ? true : false );
   }

}

// adds an item to map point, or stacks charges.
// returns false if item exceeds tile's weight limits or item count. This function is expensive, and meant for
// user initiated actions, not mapgen!
// overflow_radius > 0: if x,y is full, attempt to drop item up to overflow_radius squares away, if x,y is full
bool map::add_item_or_charges(const tripoint &p, item new_item, int overflow_radius) {

    if(!inbounds(p) ) {
        // Complain about things that should never happen.
        dbg(D_INFO) << p.x << "," << p.y << "," << p.z << ", liquid "
                    <<(new_item.made_of(LIQUID) && has_flag("SWIMMABLE", p)) <<
                    ", destroy_item "<<has_flag("DESTROY_ITEM", p);

        return false;
    }
    if( (new_item.made_of(LIQUID) && has_flag("SWIMMABLE", p)) ||
            has_flag("DESTROY_ITEM", p) || new_item.has_flag("NO_DROP") ) {
        // Silently fail on mundane things that prevent item spawn.
        return false;
    }


    bool tryaddcharges = (new_item.charges  != -1 && new_item.count_by_charges());
    std::vector<tripoint> ps = closest_tripoints_first(overflow_radius, p);
    for( const auto p_it : ps ) {
        if( !inbounds(p_it) || new_item.volume() > this->free_volume(p_it) ||
            has_flag("DESTROY_ITEM", p_it) || has_flag("NOITEM", p_it) ) {
            continue;
        }

        if( tryaddcharges ) {
            for( auto &i : i_at( p_it ) ) {
                if( i.merge_charges( new_item ) ) {
                    return true;
                }
            }
        }
        if( i_at( p_it ).size() < MAX_ITEM_IN_SQUARE ) {
            add_item( p_it, new_item );
            return true;
        }
    }
    return false;
}

// Place an item on the map, despite the parameter name, this is not necessaraly a new item.
// WARNING: does -not- check volume or stack charges. player functions (drop etc) should use
// map::add_item_or_charges
void map::add_item(const tripoint &p, item new_item)
{
    if (!inbounds( p )) {
        return;
    }
    int lx, ly;
    submap * const current_submap = get_submap_at(p, lx, ly);

    // Process foods when they are added to the map, here instead of add_item_at()
    // to avoid double processing food during active item processing.
    if( new_item.needs_processing() && new_item.is_food() ) {
        new_item.process( nullptr, p, false );
    }
    add_item_at(p, current_submap->itm[lx][ly].end(), new_item);
}

void map::add_item_at( const tripoint &p,
                       std::list<item>::iterator index, item new_item )
{
    if (new_item.made_of(LIQUID) && has_flag( "SWIMMABLE", p )) {
        return;
    }
    if (has_flag( "DESTROY_ITEM", p )) {
        return;
    }
    if (new_item.has_flag("ACT_IN_FIRE") && get_field( p, fd_fire ) != nullptr ) {
        new_item.active = true;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );
    current_submap->is_uniform = false;

    current_submap->update_lum_add(new_item, lx, ly);
    const auto new_pos = current_submap->itm[lx][ly].insert( index, new_item );
    if( new_item.needs_processing() ) {
        current_submap->active_items.add( new_pos, point(lx, ly) );
    }
}

item map::water_from(const tripoint &p)
{
    item ret("water", 0);
    if (ter( p ) == t_water_sh && one_in(3))
        ret.poison = rng(1, 4);
    else if (ter( p ) == t_water_dp && one_in(4))
        ret.poison = rng(1, 4);
    else if (ter( p ) == t_sewage)
        ret.poison = rng(1, 7);
    return ret;
}
item map::swater_from(const tripoint &p)
{
    (void)p;
    item ret("salt_water", 0);

    return ret;
}

// Check if it's in a fridge and is food, set the fridge
// date to current time, and also check contents.
static void apply_in_fridge(item &it)
{
    if (it.is_food() && it.fridge == 0) {
        it.fridge = (int) calendar::turn;
        // cool down of the HOT flag, is unsigned, don't go below 1
        if ((it.has_flag("HOT")) && (it.item_counter > 10)) {
            it.item_counter -= 10;
        }
        // This sets the COLD flag, and doesn't go above 600
        if ((it.has_flag("EATEN_COLD")) && (!it.has_flag("COLD"))) {
            it.item_tags.insert("COLD");
            it.active = true;
        }
        if ((it.has_flag("COLD")) && (it.item_counter <= 590) && it.fridge > 0) {
            it.item_counter += 10;
        }
    }
    if (it.is_container()) {
        for( auto &elem : it.contents ) {
            apply_in_fridge( elem );
        }
    }
}

template <typename Iterator>
static bool process_item( item_stack &items, Iterator &n, const tripoint &location, bool activate )
{
    // make a temporary copy, remove the item (in advance)
    // and use that copy to process it
    item temp_item = *n;
    auto insertion_point = items.erase( n );
    if( !temp_item.process( nullptr, location, activate ) ) {
        // Not destroyed, must be inserted again.
        // If the item lost its active flag in processing,
        // it won't be re-added to the active list, tidy!
        // Re-insert at the item's previous position.
        // This assumes that the item didn't invalidate any iterators
        // As a result of activation, because everything that does that
        // destroys itself.
        items.insert_at( insertion_point, temp_item );
        return false;
    }
    return true;
}

static bool process_map_items( item_stack &items, std::list<item>::iterator &n, 
                               const tripoint &location, std::string )
{
    return process_item( items, n, location, false );
}

static void process_vehicle_items( vehicle *cur_veh, int part )
{
    const bool fridge_here = cur_veh->fridge_on && cur_veh->part_flag(part, VPFLAG_FRIDGE);
    if( fridge_here ) {
        for( auto &n : cur_veh->get_items( part ) ) {
            apply_in_fridge(n);
        }
    }
    if( cur_veh->recharger_on && cur_veh->part_with_feature(part, VPFLAG_RECHARGE) >= 0 ) {
        for( auto &n : cur_veh->get_items( part ) ) {
            if( !n.has_flag("RECHARGE") ) {
                continue;
            }
            int full_charge = dynamic_cast<it_tool*>(n.type)->max_charges;
            if( n.has_flag("DOUBLE_AMMO") ) {
                full_charge = full_charge * 2;
            }
            if( n.is_tool() && full_charge > n.charges ) {
                if( one_in(10) ) {
                    n.charges++;
                }
            }
        }
    }
}

void map::process_active_items()
{
    process_items( true, process_map_items, std::string {} );
}

template<typename T>
void map::process_items( bool const active, T processor, std::string const &signal )
{
    // TODO: Z
    const int gz = abs_sub.z;
    tripoint gp( 0, 0, gz );
    int &gx = gp.x;
    int &gy = gp.y;
    for( gx = 0; gx < my_MAPSIZE; ++gx ) {
        for( gy = 0; gy < my_MAPSIZE; ++gy ) {
            submap *const current_submap = get_submap_at_grid( gp );
            // Vehicles first in case they get blown up and drop active items on the map.
            if( !current_submap->vehicles.empty() ) {
                process_items_in_vehicles(current_submap, processor, signal);
            }
            if( !active || !current_submap->active_items.empty() ) {
                process_items_in_submap(current_submap, gp, processor, signal);
            }
        }
    }
}

template<typename T>
void map::process_items_in_submap( submap *const current_submap, 
                                   const tripoint &gridp,
                                   T processor, std::string const &signal )
{
    // Get a COPY of the active item list for this submap.
    // If more are added as a side effect of processing, they are ignored this turn.
    // If they are destroyed before processing, they don't get processed.
    std::list<item_reference> active_items = current_submap->active_items.get();
    auto const grid_offset = point {gridp.x * SEEX, gridp.y * SEEY};
    for( auto &active_item : active_items ) {
        if( !current_submap->active_items.has( active_item ) ) {
            continue;
        }

        const tripoint map_location = tripoint( grid_offset + active_item.location, gridp.z );
        auto items = i_at( map_location );
        processor( items, active_item.item_iterator, map_location, signal );
    }
}

template<typename T>
void map::process_items_in_vehicles( submap *const current_submap, T processor,
                                     std::string const &signal )
{
    std::vector<vehicle*> const &veh_in_nonant = current_submap->vehicles;
    // a copy, important if the vehicle list changes because a
    // vehicle got destroyed by a bomb (an active item!), this list
    // won't change, but veh_in_nonant will change.
    std::vector<vehicle*> const vehicles = veh_in_nonant;
    for( auto &cur_veh : vehicles ) {
        if (std::find(begin(veh_in_nonant), end(veh_in_nonant), cur_veh) == veh_in_nonant.end()) {
            // vehicle not in the vehicle list of the nonant, has been
            // destroyed (or moved to another nonant?)
            // Can't be sure that it still exists, so skip it
            continue;
        }

        process_items_in_vehicle( cur_veh, current_submap, processor, signal );
    }
}

template<typename T>
void map::process_items_in_vehicle( vehicle *const cur_veh, submap *const current_submap,
                                    T processor, std::string const &signal )
{
    std::vector<int> cargo_parts = cur_veh->all_parts_with_feature(VPFLAG_CARGO, true);
    for( int part : cargo_parts ) {
        process_vehicle_items( cur_veh, part );
    }

    for( auto &active_item : cur_veh->active_items.get() ) {
        if ( cargo_parts.empty() ) {
            return;
        } else if( !cur_veh->active_items.has( active_item ) ) {
            continue;
        }

        auto const it = std::find_if(begin(cargo_parts), end(cargo_parts), [&](int const part) {
            return active_item.location == cur_veh->parts[static_cast<size_t>(part)].mount;
        });

        if (it == std::end(cargo_parts)) {
            continue; // Can't find a cargo part matching the active item.
        }

        // Find the cargo part and coordinates corresponding to the current active item.
        auto const part_index = static_cast<size_t>(*it);
        const point partloc = cur_veh->global_pos() + cur_veh->parts[part_index].precalc[0];
        // TODO: Make this 3D when vehicles know their Z coord
        const tripoint item_location = tripoint( partloc, abs_sub.z );
        auto items = cur_veh->get_items(static_cast<int>(part_index));
        if(!processor(items, active_item.item_iterator, item_location, signal)) {
            // If the item was NOT destroyed, we can skip the remainder,
            // which handles fallout from the vehicle being damaged.
            continue;
        }

        // item does not exist anymore, might have been an exploding bomb,
        // check if the vehicle is still valid (does exist)
        auto const &veh_in_nonant = current_submap->vehicles;
        if(std::find(begin(veh_in_nonant), end(veh_in_nonant), cur_veh) == veh_in_nonant.end()) {
            // Nope, vehicle is not in the vehicle list of the submap,
            // it might have moved to another submap (unlikely)
            // or be destroyed, anywaay it does not need to be processed here
            return;
        }

        // Vehicle still valid, reload the list of cargo parts,
        // the list of cargo parts might have changed (imagine a part with
        // a low index has been removed by an explosion, all the other
        // parts would move up to fill the gap).
        cargo_parts = cur_veh->all_parts_with_feature(VPFLAG_CARGO, false);
    }
}

// Crafting/item finding functions
bool map::sees_some_items( const tripoint &p, const player &u )
{
    // can only see items if there are any items.
    return !i_at( p ).empty() && could_see_items( p, u );
}

bool map::could_see_items( const tripoint &p, const player &u ) const
{
    const bool container = has_flag_ter_or_furn( "CONTAINER", p );
    const bool sealed = has_flag_ter_or_furn( "SEALED", p );
    if( sealed && container ) {
        // never see inside of sealed containers
        return false;
    }
    if( container ) {
        // can see inside of containers if adjacent or
        // on top of the container
        return ( abs( p.x - u.posx() ) <= 1 && 
                 abs( p.y - u.posy() ) <= 1 &&
                 abs( p.z - u.posz() ) <= 1 );
    }
    return true;
}

template <typename Stack>
std::list<item> use_amount_stack( Stack stack, const itype_id type, int &quantity,
                                const bool use_container )
{
    std::list<item> ret;
    for( auto a = stack.begin(); a != stack.end() && quantity > 0; ) {
        if( a->use_amount(type, quantity, use_container, ret) ) {
            a = stack.erase( a );
        } else {
            ++a;
        }
    }
    return ret;
}

std::list<item> map::use_amount_square( const tripoint &p, const itype_id type,
                                        int &quantity, const bool use_container )
{
    std::list<item> ret;
    int vpart = -1;
    vehicle *veh = veh_at( p, vpart );

    if( veh ) {
        const int cargo = veh->part_with_feature(vpart, "CARGO");
        if( cargo >= 0 ) {
            std::list<item> tmp = use_amount_stack( veh->get_items(cargo), type,
                                                    quantity, use_container );
            ret.splice( ret.end(), tmp );
        }
    }
    std::list<item> tmp = use_amount_stack( i_at( p ), type, quantity, use_container );
    ret.splice( ret.end(), tmp );
    return ret;
}

std::list<item> map::use_amount( const tripoint &origin, const int range, const itype_id type,
                                 const int amount, const bool use_container )
{
    std::list<item> ret;
    int quantity = amount;
    for( int radius = 0; radius <= range && quantity > 0; radius++ ) {
        tripoint p( origin.x - radius, origin.y - radius, origin.z );
        int &x = p.x;
        int &y = p.y;
        for( x = origin.x - radius; x <= origin.x + radius; x++ ) {
            for( y = origin.y - radius; y <= origin.y + radius; y++ ) {
                if( rl_dist( origin, p ) >= radius ) {
                    std::list<item> tmp;
                    tmp = use_amount_square( p , type, quantity, use_container );
                    ret.splice( ret.end(), tmp );
                }
            }
        }
    }
    return ret;
}

template <typename Stack>
std::list<item> use_charges_from_stack( Stack stack, const itype_id type, long &quantity)
{
    std::list<item> ret;
    for( auto a = stack.begin(); a != stack.end() && quantity > 0; ) {
        if( a->use_charges(type, quantity, ret) ) {
            a = stack.erase( a );
        } else {
            ++a;
        }
    }
    return ret;
}

long remove_charges_in_list(const itype *type, map_stack stack, long quantity)
{
    auto target = stack.begin();
    for( ; target != stack.end(); ++target ) {
        if( target->type == type ) {
            break;
        }
    }

    if( target != stack.end() ) {
        if( target->charges > quantity) {
            target->charges -= quantity;
            return quantity;
        } else {
            const long charges = target->charges;
            target->charges = 0;
            if( target->destroyed_at_zero_charges() ) {
                stack.erase( target );
            }
            return charges;
        }
    }
    return 0;
}

void use_charges_from_furn( const furn_t &f, const itype_id &type, long &quantity,
                            map *m, const tripoint &p, std::list<item> &ret )
{
    itype *itt = f.crafting_pseudo_item_type();
    if (itt == NULL || itt->id != type) {
        return;
    }
    const itype *ammo = f.crafting_ammo_item_type();
    if (ammo != NULL) {
        item furn_item(itt->id, 0);
        furn_item.charges = remove_charges_in_list(ammo, m->i_at( p ), quantity);
        if (furn_item.charges > 0) {
            ret.push_back(furn_item);
            quantity -= furn_item.charges;
        }
    }
}

std::list<item> map::use_charges(const tripoint &origin, const int range,
                                 const itype_id type, const long amount)
{
    std::list<item> ret;
    long quantity = amount;
    for( int radius = 0; radius <= range && quantity > 0; radius++ ) {
        tripoint p( origin.x - radius, origin.y - radius, origin.z );
        int &x = p.x;
        int &y = p.y;
        for( x = origin.x - radius; x <= origin.x + radius; x++ ) {
            for( y = origin.y - radius; y <= origin.y + radius; y++ ) {
                if( has_furn( p ) && accessible_furniture( origin, p, range ) ) {
                    use_charges_from_furn( furn_at( p ), type, quantity, this, p, ret );
                    if( quantity <= 0 ) {
                        return ret;
                    }
                }
                if( !accessible_items( origin, p, range) ) {
                    continue;
                }
                if( rl_dist( origin, p ) >= radius ) {
                    int vpart = -1;
                    vehicle *veh = veh_at( p, vpart );

                    if( veh ) { // check if a vehicle part is present to provide water/power
                        const int kpart = veh->part_with_feature(vpart, "KITCHEN");
                        const int weldpart = veh->part_with_feature(vpart, "WELDRIG");
                        const int craftpart = veh->part_with_feature(vpart, "CRAFTRIG");
                        const int forgepart = veh->part_with_feature(vpart, "FORGE");
                        const int chempart = veh->part_with_feature(vpart, "CHEMLAB");
                        const int cargo = veh->part_with_feature(vpart, "CARGO");

                        if (kpart >= 0) { // we have a kitchen, now to see what to drain
                            ammotype ftype = "NULL";

                            if (type == "water_clean") {
                                ftype = "water";
                            } else if (type == "hotplate") {
                                ftype = "battery";
                            }

                            item tmp(type, 0); //TODO add a sane birthday arg
                            tmp.charges = veh->drain(ftype, quantity);
                            quantity -= tmp.charges;
                            ret.push_back(tmp);

                            if (quantity == 0) {
                                return ret;
                            }
                        }

                        if (weldpart >= 0) { // we have a weldrig, now to see what to drain
                            ammotype ftype = "NULL";

                            if (type == "welder") {
                                ftype = "battery";
                            } else if (type == "soldering_iron") {
                                ftype = "battery";
                            }

                            item tmp(type, 0); //TODO add a sane birthday arg
                            tmp.charges = veh->drain(ftype, quantity);
                            quantity -= tmp.charges;
                            ret.push_back(tmp);

                            if (quantity == 0) {
                                return ret;
                            }
                        }

                        if (craftpart >= 0) { // we have a craftrig, now to see what to drain
                            ammotype ftype = "NULL";

                            if (type == "press") {
                                ftype = "battery";
                            } else if (type == "vac_sealer") {
                                ftype = "battery";
                            } else if (type == "dehydrator") {
                                ftype = "battery";
                            }

                            item tmp(type, 0); //TODO add a sane birthday arg
                            tmp.charges = veh->drain(ftype, quantity);
                            quantity -= tmp.charges;
                            ret.push_back(tmp);

                            if (quantity == 0) {
                                return ret;
                            }
                        }

                        if (forgepart >= 0) { // we have a veh_forge, now to see what to drain
                            ammotype ftype = "NULL";

                            if (type == "forge") {
                                ftype = "battery";
                            }

                            item tmp(type, 0); //TODO add a sane birthday arg
                            tmp.charges = veh->drain(ftype, quantity);
                            quantity -= tmp.charges;
                            ret.push_back(tmp);

                            if (quantity == 0) {
                                return ret;
                            }
                        }

                        if (chempart >= 0) { // we have a chem_lab, now to see what to drain
                            ammotype ftype = "NULL";

                            if (type == "chemistry_set") {
                                ftype = "battery";
                            } else if (type == "hotplate") {
                                ftype = "battery";
                            }

                            item tmp(type, 0); //TODO add a sane birthday arg
                            tmp.charges = veh->drain(ftype, quantity);
                            quantity -= tmp.charges;
                            ret.push_back(tmp);

                            if (quantity == 0) {
                                return ret;
                            }
                        }

                        if (cargo >= 0) {
                            std::list<item> tmp =
                                use_charges_from_stack( veh->get_items(cargo), type, quantity );
                            ret.splice(ret.end(), tmp);
                            if (quantity <= 0) {
                                return ret;
                            }
                        }
                    }

                    std::list<item> tmp = use_charges_from_stack( i_at( p ), type, quantity );
                    ret.splice(ret.end(), tmp);
                    if (quantity <= 0) {
                        return ret;
                    }
                }
            }
        }
    }
    return ret;
}

std::list<std::pair<tripoint, item *> > map::get_rc_items( int x, int y, int z )
{
    std::list<std::pair<tripoint, item *> > rc_pairs;
    tripoint pos;
    (void)z;
    pos.z = abs_sub.z;
    for( pos.x = 0; pos.x < SEEX * MAPSIZE; pos.x++ ) {
        if( x != -1 && x != pos.x ) {
            continue;
        }
        for( pos.y = 0; pos.y < SEEY * MAPSIZE; pos.y++ ) {
            if( y != -1 && y != pos.y ) {
                continue;
            }
            auto items = i_at( pos );
            for( auto &elem : items ) {
                if( elem.has_flag( "RADIO_ACTIVATION" ) || elem.has_flag( "RADIO_CONTAINER" ) ) {
                    rc_pairs.push_back( std::make_pair( pos, &( elem ) ) );
                }
            }
        }
    }

    return rc_pairs;
}

static bool trigger_radio_item( item_stack &items, std::list<item>::iterator &n, 
                                const tripoint &pos,
                                std::string signal )
{
    bool trigger_item = false;
    // Check for charges != 0 not >0, so that -1 charge tools can still be used
    if( n->charges != 0 && n->has_flag("RADIO_ACTIVATION") && n->has_flag(signal) ) {
        sounds::sound(pos.x, pos.y, 6, "beep.");
        if( n->has_flag("RADIO_INVOKE_PROC") ) {
            // Invoke twice: first to transform, then later to proc
            process_item( items, n, pos, true );
            n->charges = 0;
        }
        if( n->has_flag("BOMB") ) {
            // Set charges to 0 to ensure it detonates.
            n->charges = 0;
        }
        trigger_item = true;
    } else if( n->has_flag("RADIO_CONTAINER") && !n->contents.empty() &&
               n->contents[0].has_flag( signal ) ) {
        // A bomb is the only thing meaningfully placed in a container,
        // If that changes, this needs logic to handle the alternative.
        itype_id bomb_type = n->contents[0].type->id;

        n->make(bomb_type);
        n->charges = 0;
        trigger_item = true;
    }
    if( trigger_item ) {
        return process_item( items, n, pos, true ); 
    }
    return false;
}

void map::trigger_rc_items( std::string signal )
{
    process_items( false, trigger_radio_item, signal );
}

item *map::item_from( const tripoint &pos, size_t index ) {
    auto items = i_at( pos );

    if( index >= items.size() ) {
        return nullptr;
    } else {
        return &items[index];
    }
}

item *map::item_from( vehicle *veh, int cargo_part, size_t index ) {
   auto items = veh->get_items( cargo_part );

    if( index >= items.size() ) {
        return nullptr;
    } else {
        return &items[index];
    }
}

// Traps: 2D
void map::trap_set(const int x, const int y, const std::string & sid)
{
    trap_set( tripoint( x, y, abs_sub.z ), sid );
}

void map::trap_set( const tripoint &p, const std::string & sid)
{
    if( trapmap.find( sid ) == trapmap.end() ) {
        return;
    }

    add_trap( p, (trap_id)trapmap[sid] );
}

void map::trap_set(const int x, const int y, const trap_id id)
{
    trap_set( tripoint( x, y, abs_sub.z ), id );
}

void map::trap_set( const tripoint &p, const trap_id id)
{
    add_trap( p, id );
}

// todo: to be consistent with ???_at(...) this should return ref to the actual trap object
const trap &map::tr_at( const int x, const int y ) const
{
    return tr_at( tripoint( x, y, abs_sub.z ) );
}

const trap &map::tr_at( const tripoint &p ) const
{
    if( !inbounds( p.x, p.y, p.z ) ) {
        return *traplist[tr_null];
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    if (terlist[ current_submap->get_ter( lx, ly ) ].trap != tr_null) {
        return *traplist[terlist[ current_submap->get_ter( lx, ly ) ].trap];
    }

    return *traplist[current_submap->get_trap( lx, ly )];
}

void map::add_trap(const int x, const int y, const trap_id t)
{
    add_trap( tripoint( x, y, abs_sub.z ), t );
}

void map::add_trap( const tripoint &p, const trap_id t)
{
    if( !inbounds( p ) ) 
    { 
        return;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );
    const ter_t &ter = terlist[ current_submap->get_ter( lx, ly ) ];
    if( ter.trap != tr_null ) {
        debugmsg( "set trap %s on top of terrain %s which already has a builit-in trap",
                  traplist[t]->name.c_str(), ter.name.c_str() );
        return;
    }

    // If there was already a trap here, remove it.
    if( current_submap->get_trap( lx, ly ) != tr_null ) {
        remove_trap( p );
    }

    current_submap->set_trap( lx, ly, t );
    if( t != tr_null ) {
        traplocs[t].push_back( p );
    }
}

void map::disarm_trap( const int x, const int y )
{
    disarm_trap( tripoint( x, y, abs_sub.z ) );
}

void map::disarm_trap( const tripoint &p )
{
    int skillLevel = g->u.skillLevel("traps");

    const trap &tr = tr_at( p );
    if( tr.is_null() ) {
        debugmsg( "Tried to disarm a trap where there was none (%d %d %d)", p.x, p.y, p.z );
        return;
    }

    const int tSkillLevel = g->u.skillLevel("traps");
    const int diff = tr.get_difficulty();
    int roll = rng(tSkillLevel, 4 * tSkillLevel);

    // Some traps are not actual traps. Skip the rolls, different message and give the option to grab it right away.
    if( tr.get_avoidance() ==  0 && tr.get_difficulty() == 0 ) {
        add_msg(_("You take down the %s."), tr.name.c_str());
        tr.on_disarmed( p );
        return;
    }

    while ((rng(5, 20) < g->u.per_cur || rng(1, 20) < g->u.dex_cur) && roll < 50) {
        roll++;
    }
    if (roll >= diff) {
        add_msg(_("You disarm the trap!"));
        tr.on_disarmed( p );
        if(diff > 1.25 * skillLevel) { // failure might have set off trap
            g->u.practice( "traps", 1.5*(diff - skillLevel) );
        }
    } else if (roll >= diff * .8) {
        add_msg(_("You fail to disarm the trap."));
        if(diff > 1.25 * skillLevel) {
            g->u.practice( "traps", 1.5*(diff - skillLevel) );
        }
    } else {
        add_msg(m_bad, _("You fail to disarm the trap, and you set it off!"));
        tr.trigger( p, &g->u );
        if(diff - roll <= 6) {
            // Give xp for failing, but not if we failed terribly (in which
            // case the trap may not be disarmable).
            g->u.practice( "traps", 2*diff );
        }
    }
}

void map::remove_trap(const int x, const int y)
{
    remove_trap( tripoint( x, y, abs_sub.z ) );
}

void map::remove_trap( const tripoint &p )
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    trap_id t = current_submap->get_trap(lx, ly);
    if (t != tr_null) {
        if( g != nullptr && this == &g->m ) {
            g->u.add_known_trap( p, "tr_null");
        }

        current_submap->set_trap(lx, ly, tr_null);
        auto &traps = traplocs[t];
        const auto iter = std::find( traps.begin(), traps.end(), p );
        if( iter != traps.end() ) {
            traps.erase( iter );
        }
    }
}
/*
 * Get wrapper for all fields at xyz
 */
const field &map::field_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        nulfield = field();
        return nulfield;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->fld[lx][ly];
}

/*
 * As above, except not const
 */
field &map::field_at( const tripoint &p )
{
    if( !inbounds( p ) ) {
        nulfield = field();
        return nulfield;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->fld[lx][ly];
}

int map::adjust_field_age( const tripoint &p, const field_id t, const int offset ) {
    return set_field_age( p, t, offset, true);
}

int map::adjust_field_strength( const tripoint &p, const field_id t, const int offset ) {
    return set_field_strength( p, t, offset, true );
}

/*
 * Set age of field type at point, or increment/decrement if offset=true
 * returns resulting age or -1 if not present.
 */
int map::set_field_age( const tripoint &p, const field_id t, const int age, bool isoffset ) {
    field_entry *field_ptr = get_field( p, t );
    if( field_ptr != nullptr ) {
        int adj = ( isoffset ? field_ptr->getFieldAge() : 0 ) + age;
        field_ptr->setFieldAge( adj );
        return adj;
    }

    return -1;
}

/*
 * set strength of field type at point, creating if not present, removing if strength is 0
 * returns resulting strength, or 0 for not present
 */
int map::set_field_strength( const tripoint &p, const field_id t, const int str, bool isoffset ) {
    field_entry * field_ptr = get_field( p, t );
    if( field_ptr != nullptr ) {
        int adj = ( isoffset ? field_ptr->getFieldDensity() : 0 ) + str;
        if( adj > 0 ) {
            field_ptr->setFieldDensity( adj );
            return adj;
        } else {
            remove_field( p, t );
            return 0;
        }
    } else if( 0 + str > 0 ) {
        return ( add_field( p, t, str, 0 ) ? str : 0 );
    }

    return 0;
}

int map::get_field_age( const tripoint &p, const field_id t ) const
{
    auto field_ptr = field_at( p ).findField( t );
    return ( field_ptr == nullptr ? -1 : field_ptr->getFieldAge() );
}

int map::get_field_strength( const tripoint &p, const field_id t ) const
{
    auto field_ptr = field_at( p ).findField( t );
    return ( field_ptr == nullptr ? 0 : field_ptr->getFieldDensity() );
}

field_entry *map::get_field( const tripoint &p, const field_id t ) {
    if( !inbounds( p ) ) {
        return nullptr;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );

    return current_submap->fld[lx][ly].findField( t );
}

bool map::add_field(const tripoint &p, const field_id t, int density, const int age)
{
    if( !inbounds( p ) ) {
        return false;
    }

    if( density > 3) {
        density = 3;
    }

    if( density <= 0) {
        return false;
    }

    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    current_submap->is_uniform = false;

    if( current_submap->fld[lx][ly].addField( t, density, age ) ) {
        // TODO: Update overall field_count appropriately.
        // This is the spirit of "fd_null" that it used to be.
        current_submap->field_count++; //Only adding it to the count if it doesn't exist.
    }

    if( g != nullptr && this == &g->m && p == g->u.pos3() ) {
        creature_in_field( g->u ); //Hit the player with the field if it spawned on top of them.
    }

    return true;
}

void map::remove_field( const tripoint &p, const field_id field_to_remove )
{
    if( !inbounds( p ) ) {
        return;
    }

    int lx, ly;
    submap * const current_submap = get_submap_at( p, lx, ly );

    if( current_submap->fld[lx][ly].findField( field_to_remove ) ) { //same as checking for fd_null in the old system
        current_submap->field_count--;
    }

    current_submap->fld[lx][ly].removeField(field_to_remove);
}

computer* map::computer_at( const tripoint &p )
{
    if( !inbounds( p ) ) {
        return nullptr;
    }

    submap * const current_submap = get_submap_at( p );

    if( current_submap->comp.name.empty() ) {
        return nullptr;
    }

    return &(current_submap->comp);
}

bool map::allow_camp( const tripoint &p, const int radius)
{
    return camp_at( p, radius ) == nullptr;
}

// locate the nearest camp in some radius (default CAMPSIZE)
basecamp* map::camp_at( const tripoint &p, const int radius)
{
    if( !inbounds( p ) ) {
        return nullptr;
    }

    const int sx = std::max(0, p.x / SEEX - radius);
    const int sy = std::max(0, p.y / SEEY - radius);
    const int ex = std::min(MAPSIZE - 1, p.x / SEEX + radius);
    const int ey = std::min(MAPSIZE - 1, p.y / SEEY + radius);

    for (int ly = sy; ly < ey; ++ly) {
        for (int lx = sx; lx < ex; ++lx) {
            submap * const current_submap = get_submap_at( p );
            if( current_submap->camp.is_valid() ) {
                // we only allow on camp per size radius, kinda
                return &(current_submap->camp);
            }
        }
    }

    return nullptr;
}

void map::add_camp( const tripoint &p, const std::string& name )
{
    if( !allow_camp( p ) ) {
        dbg(D_ERROR) << "map::add_camp: Attempting to add camp when one in local area.";
        return;
    }

    get_submap_at( p )->camp = basecamp( name, p.x, p.y );
}

void map::debug()
{
 mvprintw(0, 0, "MAP DEBUG");
 getch();
 for (int i = 0; i <= SEEX * 2; i++) {
  for (int j = 0; j <= SEEY * 2; j++) {
   if (i_at(i, j).size() > 0) {
    mvprintw(1, 0, "%d, %d: %d items", i, j, i_at(i, j).size());
    mvprintw(2, 0, "%c, %d", i_at(i, j)[0].symbol(), i_at(i, j)[0].color());
    getch();
   }
  }
 }
 getch();
}

void map::update_visibility_cache( visibility_variables &cache) {
    cache.g_light_level = (int)g->light_level();
    cache.natural_sight_range = g->u.sight_range(1);
    cache.light_sight_range = g->u.sight_range(cache.g_light_level);
    cache.lowlight_sight_range = std::max(cache.g_light_level / 2,
                                          cache.natural_sight_range);
    cache.max_sight_range = g->u.unimpaired_range();
    cache.u_clairvoyance = g->u.clairvoyance();
    cache.u_sight_impaired = g->u.sight_impaired();
    cache.bio_night_active = g->u.has_active_bionic("bio_night");
    
    cache.u_is_boomered = g->u.has_effect("boomered");
    
    for( int x = 0; x < MAPSIZE * SEEX; x++ ) {
        for( int y = 0; y < MAPSIZE * SEEY; y++ ) {
            visibility_cache[x][y] = apparent_light_at(x, y, cache);
        }
    }
}

lit_level map::apparent_light_at(int x, int y, const visibility_variables &cache) {
    const int dist = rl_dist(g->u.posx(), g->u.posy(), x, y);

    int sight_range = cache.light_sight_range;
    int low_sight_range = cache.lowlight_sight_range;

    // While viewing indoor areas use lightmap model
    if (!is_outside(x, y)) {
        sight_range = cache.natural_sight_range;

    // Don't display area as shadowy if it's outside and illuminated by natural light
    // and illuminated by source of light
    } else if (light_at(x, y) > LL_LOW || dist <= cache.light_sight_range) {
        low_sight_range = std::max(cache.g_light_level, cache.natural_sight_range);
    }

    int real_max_sight_range = std::max(cache.light_sight_range, cache.max_sight_range);
    int distance_to_look = DAYLIGHT_LEVEL;

    bool can_see = pl_sees( x, y, distance_to_look );
    lit_level lit = light_at(x, y);

    // now we're gonna adjust real_max_sight, to cover some nearby "highlights",
    // but at the same time changing light-level depending on distance,
    // to create actual "gradual" stuff
    // Also we'll try to ALWAYS show LL_BRIGHT stuff independent of where it is...
    if (lit != LL_BRIGHT) {
        if (dist > real_max_sight_range) {
            int intLit = (int)lit - (dist - real_max_sight_range)/2;
            if (intLit < 0) intLit = LL_DARK;
            lit = (lit_level)intLit;
        }
    }

    // additional case for real_max_sight_range
    // if both light_sight_range and max_sight_range were small
    // it means we really have limited visibility (e.g. inside a pit)
    // and we shouldn't touch that
    if (lit > LL_DARK && real_max_sight_range > 1) {
        real_max_sight_range = distance_to_look;
    }

    if ((cache.bio_night_active && dist < 15 && dist > cache.natural_sight_range) || // if bio_night active, blackout 15 tile radius around player
        dist > real_max_sight_range || // too far away, no matter what
        (dist > cache.light_sight_range &&
            (lit == LL_DARK ||
                (cache.u_sight_impaired && lit != LL_BRIGHT) ||
                !can_see))) { // blind
        return LL_DARK;
    } else if (dist > cache.light_sight_range && cache.u_sight_impaired && lit == LL_BRIGHT) {
        return LL_BRIGHT_ONLY;
    } else if (dist <= cache.u_clairvoyance || can_see) {
        if ( lit == LL_BRIGHT ) {
            return LL_BRIGHT;
        } else {
            if ( (dist > low_sight_range && LL_LIT > lit) || (dist > sight_range && LL_LOW == lit) ) {
                return LL_LOW;
            } else {
                return LL_LIT;
            }
        }
    }
    return LL_BLANK;
}

visibility_type map::get_visibility( const lit_level ll, const visibility_variables &cache ) const {
    switch (ll) {
    case LL_DARK: // can't see this square at all
        if( cache.u_is_boomered ) {
            return VIS_BOOMER_DARK;
        } else {
            return VIS_DARK;
        }
    case LL_BRIGHT_ONLY: // can only tell that this square is bright
        if( cache.u_is_boomered ) {
            return VIS_BOOMER;
        } else {
            return VIS_LIT;
        }
    case LL_LOW: // low light, square visible in monochrome
    case LL_LIT: // normal light
    case LL_BRIGHT: // bright light
        return VIS_CLEAR;
    case LL_BLANK:
        return VIS_HIDDEN;
    }
    return VIS_HIDDEN;
}

bool map::apply_vision_effects( WINDOW *w, const point center, int x, int y,
                                lit_level ll, const visibility_variables &cache ) const {
    int symbol = ' ';
    nc_color color = c_black;

    switch( get_visibility(ll, cache) ) {
        case VIS_DARK: // can't see this square at all
            symbol = '#';
            color = c_dkgray;
            break;
        case VIS_CLEAR:
            // Drew the tile, so bail out now.
            return false;
        case VIS_LIT: // can only tell that this square is bright
            symbol = '#';
            color = c_ltgray;
            break;
        case VIS_BOOMER:
          symbol = '#';
          color = c_pink;
            break;
        case VIS_BOOMER_DARK:
          symbol = '#';
          color = c_magenta;
            break;
        case VIS_HIDDEN:
            symbol = ' ';
            color = c_black;
            break;
    }
    mvwputch( w, y + getmaxy(w) / 2 - center.y,
              x + getmaxx(w) / 2 - center.x, color, symbol );
    return true;
}

void map::draw(WINDOW* w, const point center)
{
    // We only need to draw anything if we're not in tiles mode.
    if(is_draw_tiles_mode()) {
        return;
    }

    g->reset_light_level();

    visibility_variables cache;
    update_visibility_cache( cache );

    for( int x = center.x - getmaxx(w)/2; x <= center.x + getmaxx(w)/2; x++ ) {
        for( int y = center.y - getmaxy(w)/2; y <= center.y + getmaxy(w)/2; y++ ) {
            const lit_level lighting = visibility_cache[x][y];
            if( !apply_vision_effects( w, center, x, y, lighting, cache ) ) {
                drawsq( w, g->u, x, y, false, true, center.x, center.y,
                        lighting == LL_LOW, lighting == LL_BRIGHT );
            }
        }
    }

    g->draw_critter( g->u, center );
}

void map::drawsq(WINDOW* w, player &u, const int x, const int y, const bool invert_arg,
                 const bool show_items_arg, const int view_center_x_arg, const int view_center_y_arg,
                 const bool low_light, const bool bright_light)
{
    // We only need to draw anything if we're not in tiles mode.
    if(is_draw_tiles_mode()) {
        return;
    }

    const tripoint p( x, y, abs_sub.z );
    bool invert = invert_arg;
    bool show_items = show_items_arg;
    int cx = view_center_x_arg;
    int cy = view_center_y_arg;
    if (!INBOUNDS(x, y))
        return; // Out of bounds
    if (cx == -1)
        cx = u.posx();
    if (cy == -1)
        cy = u.posy();
    const int k = x + getmaxx(w)/2 - cx;
    const int j = y + getmaxy(w)/2 - cy;
    nc_color tercol;
    const ter_t &curr_ter = ter_at(x,y);
    const furn_t &curr_furn = furn_at(x,y);
    const trap &curr_trap = tr_at(x, y);
    const field &curr_field = field_at(x, y);
    auto curr_items = i_at(x, y);
    long sym;
    bool hi = false;
    bool graf = false;
    bool draw_item_sym = false;
    static const long AUTO_WALL_PLACEHOLDER = 2; // this should never appear as a real symbol!

    if( curr_furn.loadid != f_null ) {
        sym = curr_furn.sym;
        tercol = curr_furn.color;
    } else {
        if( curr_ter.has_flag( TFLAG_AUTO_WALL_SYMBOL ) ) {
            // If the terrain symbol is later overriden by something, we don't need to calculate
            // the wall symbol at all. This case will be detected by comparing sym to this
            // placeholder, if it's still the same, we have to calculate the wall symbol.
            sym = AUTO_WALL_PLACEHOLDER;
        } else {
            sym = curr_ter.sym;
        }
        tercol = curr_ter.color;
    }
    if (has_flag(TFLAG_SWIMMABLE, x, y) && has_flag(TFLAG_DEEP_WATER, x, y) && !u.is_underwater()) {
        show_items = false; // Can only see underwater items if WE are underwater
    }
    // If there's a trap here, and we have sufficient perception, draw that instead
    if( curr_trap.can_see( tripoint( x, y, abs_sub.z ), g->u ) ) {
        tercol = curr_trap.color;
        if (curr_trap.sym == '%') {
            switch(rng(1, 5)) {
            case 1: sym = '*'; break;
            case 2: sym = '0'; break;
            case 3: sym = '8'; break;
            case 4: sym = '&'; break;
            case 5: sym = '+'; break;
            }
        } else {
            sym = curr_trap.sym;
        }
    }
    if (curr_field.fieldCount() > 0) {
        const field_id& fid = curr_field.fieldSymbol();
        const field_entry* fe = curr_field.findField(fid);
        const field_t& f = fieldlist[fid];
        if (f.sym == '&' || fe == NULL) {
            // Do nothing, a '&' indicates invisible fields.
        } else if (f.sym == '*') {
            // A random symbol.
            switch (rng(1, 5)) {
            case 1: sym = '*'; break;
            case 2: sym = '0'; break;
            case 3: sym = '8'; break;
            case 4: sym = '&'; break;
            case 5: sym = '+'; break;
            }
        } else {
            // A field symbol '%' indicates the field should not hide
            // items/terrain. When the symbol is not '%' it will
            // hide items (the color is still inverted if there are items,
            // but the tile symbol is not changed).
            // draw_item_sym indicates that the item symbol should be used
            // even if sym is not '.'.
            // As we don't know at this stage if there are any items
            // (that are visible to the player!), we always set the symbol.
            // If there are items and the field does not hide them,
            // the code handling items will override it.
            draw_item_sym = (f.sym == '%');
            // If field priority is > 1, and the field is set to hide items,
            //draw the field as it obscures what's under it.
            if( (f.sym != '%' && f.priority > 1) || (f.sym != '%' && sym == '.'))  {
                // default terrain '.' and
                // non-default field symbol -> field symbol overrides terrain
                sym = f.sym;
            }
            tercol = f.color[fe->getFieldDensity() - 1];
        }
    }

    // If there are items here, draw those instead
    if (show_items && sees_some_items( p, g->u)) {
        // if there's furniture/terrain/trap/fields (sym!='.')
        // and we should not override it, then only highlight the square
        if (sym != '.' && sym != '%' && !draw_item_sym) {
            hi = true;
        } else {
            // otherwise override with the symbol of the last item
            sym = curr_items[curr_items.size() - 1].symbol();
            if (!draw_item_sym) {
                tercol = curr_items[curr_items.size() - 1].color();
            }
            if (curr_items.size() > 1) {
                invert = !invert;
            }
        }
    }

    int veh_part = 0;
    vehicle *veh = veh_at(x, y, veh_part);
    if (veh) {
        sym = special_symbol (veh->face.dir_symbol(veh->part_sym(veh_part)));
        tercol = veh->part_color(veh_part);
    }
    // If there's graffiti here, change background color
    if( has_graffiti_at( p ) ) {
        graf = true;
    }

    //suprise, we're not done, if it's a wall adjacent to an other, put the right glyph
    if( sym == AUTO_WALL_PLACEHOLDER ) {
        sym = determine_wall_corner( x, y );
    }

    if (u.has_effect("boomered")) {
        tercol = c_magenta;
    } else if ( u.has_nv() ) {
        tercol = (bright_light) ? c_white : c_ltgreen;
    } else if (low_light) {
        tercol = c_dkgray;
    } else if (u.has_effect("darkness")) {
        tercol = c_dkgray;
    }

    if (invert) {
        mvwputch_inv(w, j, k, tercol, sym);
    } else if (hi) {
        mvwputch_hi (w, j, k, tercol, sym);
    } else if (graf) {
        mvwputch    (w, j, k, red_background(tercol), sym);
    } else {
        mvwputch    (w, j, k, tercol, sym);
    }
}

// TODO: Implement this function in FoV update
bool map::sees( const tripoint &F, const tripoint &T, const int range, int &t1, int &t2 ) const
{
    (void)t2;
    return sees( F.x, F.y, T.x, T.y, range, t1 );
}

bool map::sees( const tripoint &F, const tripoint &T, const int range ) const
{
    int t1 = 0;
    return sees( F.x, F.y, T.x, T.y, range, t1 );
}

bool map::sees( const point F, const point T, const int range, int &bresenham_slope ) const
{
    return sees( F.x, F.y, T.x, T.y, range, bresenham_slope );
}

/*
map::sees based off code by Steve Register [arns@arns.freeservers.com]
http://roguebasin.roguelikedevelopment.org/index.php?title=Simple_Line_of_Sight
*/
bool map::sees(const int Fx, const int Fy, const int Tx, const int Ty,
               const int range, int &bresenham_slope) const
{
    const int dx = Tx - Fx;
    const int dy = Ty - Fy;
    const int ax = abs(dx) * 2;
    const int ay = abs(dy) * 2;
    const int sx = SGN(dx);
    const int sy = SGN(dy);
    int x = Fx;
    int y = Fy;
    int t = 0;
    int st;

    if (range >= 0 && range < rl_dist(Fx, Fy, Tx, Ty) ) {
        return false; // Out of range!
    }
    if (ax > ay) { // Mostly-horizontal line
        st = SGN(ay - (ax / 2));
        // Doing it "backwards" prioritizes straight lines before diagonal.
        // This will help avoid creating a string of zombies behind you and will
        // promote "mobbing" behavior (zombies surround you to beat on you)
        for (bresenham_slope = abs(ay - (ax / 2)) * 2 + 1; bresenham_slope >= -1; bresenham_slope--) {
            t = bresenham_slope * st;
            x = Fx;
            y = Fy;
            do {
                if (t > 0) {
                    y += sy;
                    t -= ax;
                }
                x += sx;
                t += ay;
                if (x == Tx && y == Ty) {
                    bresenham_slope *= st;
                    return true;
                }
            } while ((trans(x, y)) && (INBOUNDS(x,y)));
        }
        return false;
    } else { // Same as above, for mostly-vertical lines
        st = SGN(ax - (ay / 2));
        for (bresenham_slope = abs(ax - (ay / 2)) * 2 + 1; bresenham_slope >= -1; bresenham_slope--) {
            t = bresenham_slope * st;
            x = Fx;
            y = Fy;
            do {
                if (t > 0) {
                    x += sx;
                    t -= ay;
                }
                y += sy;
                t += ax;
                if (x == Tx && y == Ty) {
                    bresenham_slope *= st;
     return true;
                }
            } while ((trans(x, y)) && (INBOUNDS(x,y)));
        }
        return false;
    }
    return false; // Shouldn't ever be reached, but there it is.
}

bool map::clear_path(const int Fx, const int Fy, const int Tx, const int Ty,
                     const int range, const int cost_min, const int cost_max, int &bresenham_slope) const
{
    const int dx = Tx - Fx;
    const int dy = Ty - Fy;
    const int ax = abs(dx) * 2;
    const int ay = abs(dy) * 2;
    const int sx = SGN(dx);
    const int sy = SGN(dy);
    int x = Fx;
    int y = Fy;
    int t = 0;
    int st;

    if (range >= 0 &&  range < rl_dist(Fx, Fy, Tx, Ty) ) {
        return false; // Out of range!
    }
    if (ax > ay) { // Mostly-horizontal line
        st = SGN(ay - (ax / 2));
        // Doing it "backwards" prioritizes straight lines before diagonal.
        // This will help avoid creating a string of zombies behind you and will
        // promote "mobbing" behavior (zombies surround you to beat on you)
        for (bresenham_slope = abs(ay - (ax / 2)) * 2 + 1; bresenham_slope >= -1; bresenham_slope--) {
            t = bresenham_slope * st;
            x = Fx;
            y = Fy;
            do {
                if (t > 0) {
                    y += sy;
                    t -= ax;
                }
                x += sx;
                t += ay;
                if (x == Tx && y == Ty) {
                    bresenham_slope *= st;
                    return true;
                }
            } while (move_cost(x, y) >= cost_min && move_cost(x, y) <= cost_max &&
                     INBOUNDS(x, y));
        }
        return false;
    } else { // Same as above, for mostly-vertical lines
        st = SGN(ax - (ay / 2));
        for (bresenham_slope = abs(ax - (ay / 2)) * 2 + 1; bresenham_slope >= -1; bresenham_slope--) {
            t = bresenham_slope * st;
            x = Fx;
            y = Fy;
            do {
                if (t > 0) {
                    x += sx;
                    t -= ay;
                }
                y += sy;
                t += ax;
                if (x == Tx && y == Ty) {
                    bresenham_slope *= st;
                    return true;
                }
            } while (move_cost(x, y) >= cost_min && move_cost(x, y) <= cost_max &&
                     INBOUNDS(x,y));
        }
        return false;
    }
    return false; // Shouldn't ever be reached, but there it is.
}

// TODO: Z
bool map::clear_path( const tripoint &f, const tripoint &t, const int range,
                      const int cost_min, const int cost_max, int &bres1, int &bres2 ) const
{
    if( f.z != t.z ) {
        return false;
    }

    bres2 = 0;
    return clear_path( f.x, f.y, t.x, t.y, range, cost_min, cost_max, bres1 );
}

bool map::clear_path( const tripoint &f, const tripoint &t, const int range,
                      const int cost_min, const int cost_max ) const
{
    int t1 = 0;
    int t2 = 0;
    return clear_path( f, t, range, cost_min, cost_max, t1, t2 );
}

bool map::accessible_items( const tripoint &f, const tripoint &t, const int range ) const
{
    return ( !has_flag( "SEALED", t ) || has_flag( "LIQUIDCONT", t ) ) &&
           ( f == t || clear_path( f, t, range, 1, 100 ) );
}

bool map::accessible_furniture( const tripoint &f, const tripoint &t, const int range ) const
{
    return ( f == t || clear_path( f, t, range, 1, 100 ) );
}

std::vector<point> map::getDirCircle(const int Fx, const int Fy, const int Tx, const int Ty) const
{
    std::vector<point> vCircle;
    vCircle.resize(8);

    const std::vector<point> vLine = line_to(Fx, Fy, Tx, Ty, 0);
    const std::vector<point> vSpiral = closest_points_first(1, Fx, Fy);
    const std::vector<int> vPos {1,2,4,6,8,7,5,3};

    //  All possible constelations (closest_points_first goes clockwise)
    //  753  531  312  124  246  468  687  875
    //  8 1  7 2  5 4  3 6  1 8  2 7  4 5  6 3
    //  642  864  786  578  357  135  213  421

    int iPosOffset = 0;
    for (unsigned int i = 1; i < vSpiral.size(); i++) {
        if (vSpiral[i].x == vLine[0].x && vSpiral[i].y == vLine[0].y) {
            iPosOffset = i-1;
            break;
        }
    }

    for (unsigned int i = 1; i < vSpiral.size(); i++) {
        if (iPosOffset >= (int)vPos.size()) {
            iPosOffset = 0;
        }

        vCircle[vPos[iPosOffset++]-1] = point(vSpiral[i].x, vSpiral[i].y);
    }

    return vCircle;
}

struct pair_greater_cmp
{
    bool operator()( const std::pair<int, point> &a, const std::pair<int, point> &b)
    {
        return a.first > b.first;
    }
};

std::vector<point> map::route(const int Fx, const int Fy, const int Tx, const int Ty, const int bash) const
{
    /* TODO: If the origin or destination is out of bound, figure out the closest
     * in-bounds point and go to that, then to the real origin/destination.
     */

    if( !INBOUNDS(Fx, Fy) || !INBOUNDS(Tx, Ty) ) {
        int linet;
        if (sees(Fx, Fy, Tx, Ty, -1, linet)) {
            return line_to(Fx, Fy, Tx, Ty, linet);
        } else {
            std::vector<point> empty;
            return empty;
        }
    }
    // First, check for a simple straight line on flat ground
    int linet = 0;
    if( clear_path( Fx, Fy, Tx, Ty, -1, 2, 2, linet ) ) {
        return line_to(Fx, Fy, Tx, Ty, linet);
    }
    /*
    if (move_cost(Tx, Ty) == 0) {
        debugmsg("%d:%d wanted to move to %d:%d, a %s!", Fx, Fy, Tx, Ty,
                    tername(Tx, Ty).c_str());
    }
    if (move_cost(Fx, Fy) == 0) {
        debugmsg("%d:%d, a %s, wanted to move to %d:%d!", Fx, Fy,
                    tername(Fx, Fy).c_str(), Tx, Ty);
    }
    */
    std::priority_queue< std::pair<int, point>, std::vector< std::pair<int, point> >, pair_greater_cmp > open;
    std::set<point> closed;
    astar_list list[SEEX * MAPSIZE][SEEY * MAPSIZE];
    int score[SEEX * MAPSIZE][SEEY * MAPSIZE];
    int gscore[SEEX * MAPSIZE][SEEY * MAPSIZE];
    point parent[SEEX * MAPSIZE][SEEY * MAPSIZE];
    const int pad = 8; // Should be much bigger - low value makes pathfinders dumb!
    int startx = Fx - pad, endx = Tx + pad, starty = Fy - pad, endy = Ty + pad;
    if (Tx < Fx) {
        startx = Tx - pad;
        endx = Fx + pad;
    }
    if (Ty < Fy) {
        starty = Ty - pad;
        endy = Fy + pad;
    }
    if( startx < 0 ) {
        startx = 0;
    }
    if( starty < 0 ) {
        starty = 0;
    }
    if( endx > SEEX * my_MAPSIZE - 1 ) {
        endx = SEEX * my_MAPSIZE - 1;
    }
    if( endy > SEEY * my_MAPSIZE - 1 ) {
        endy = SEEY * my_MAPSIZE - 1;
    }

    for (int x = startx; x <= endx; x++) {
        for (int y = starty; y <= endy; y++) {
            list  [x][y] = ASL_NONE; // Mark as unvisited
            score [x][y] = INT_MAX;  // Unreachable
            gscore[x][y] = INT_MAX;  // Unreachable
            parent[x][y] = point(-1, -1);
        }
    }

    open.push( std::make_pair( 0, point(Fx, Fy) ) );
    score[Fx][Fy] = 0;
    gscore[Fx][Fy] = 0;

    bool done = false;

    do {
        auto pr = open.top();
        open.pop();
        if( pr.first > 9999 ) {
            // Shortest path would be too long, return empty vector
            return std::vector<point>();
        }

        const point &cur = pr.second;
        if( list[cur.x][cur.y] == ASL_CLOSED ) {
            continue;
        }

        list[cur.x][cur.y] = ASL_CLOSED;
        std::vector<point> vDirCircle = getDirCircle( cur.x, cur.y, Tx, Ty );

        for( auto &elem : vDirCircle ) {
            const int x = elem.x;
            const int y = elem.y;

            if( x == Tx && y == Ty ) {
                done = true;
                parent[x][y] = cur;
            } else if( x >= startx && x <= endx && y >= starty && y <= endy ) {
                if( list[x][y] == ASL_CLOSED ) {
                    continue;
                }

                int part = -1;
                const furn_t &furniture = furn_at( x, y );
                const ter_t &terrain = ter_at( x, y );
                const vehicle *veh = veh_at_internal( x, y, part );

                const int cost = move_cost_internal( furniture, terrain, veh, part );
                // Don't calculate bash rating unless we intend to actually use it
                const int rating = ( bash == 0 || cost != 0 ) ? -1 :
                                     bash_rating_internal( bash, furniture, terrain, veh, part );

                if( cost == 0 && rating <= 0 && terrain.open.empty() ) {
                    list[x][y] = ASL_CLOSED; // Close it so that next time we won't try to calc costs
                    continue;
                }

                int newg = gscore[cur.x][cur.y] + cost + ((cur.x - x != 0 && cur.y - y != 0) ? 1 : 0);
                if( cost == 0 ) {
                    // Handle all kinds of doors
                    // Only try to open INSIDE doors from the inside

                    if ( !terrain.open.empty() &&
                           ( !terrain.has_flag( "OPENCLOSE_INSIDE" ) || !is_outside( cur.x, cur.y ) ) ) {
                        newg += 4; // To open and then move onto the tile
                    } else if( veh != nullptr ) {
                        part = veh->obstacle_at_part( part );
                        int dummy = -1;
                        if( !veh->part_flag( part, "OPENCLOSE_INSIDE" ) || veh_at_internal( cur.x, cur.y, dummy ) == veh ) {
                            // Handle car doors, but don't try to path through curtains
                            newg += 10; // One turn to open, 4 to move there
                        } else {
                            // Car obstacle that isn't a door
                            newg += veh->parts[part].hp / bash + 8 + 4;
                        }
                    } else if( rating > 1 ) {
                        // Expected number of turns to bash it down, 1 turn to move there
                        // and 2 turns of penalty not to trash everything just because we can
                        newg += ( 20 / rating ) + 2 + 4; 
                    } else if( rating == 1 ) {
                        // Desperate measures, avoid whenever possible
                        newg += 1000;
                    } else {
                        newg = 10000; // Unbashable and unopenable from here
                    }
                }

                // If not in list, add it
                // If in list, add it only if we can do so with better score
                if( list[x][y] == ASL_NONE || newg < gscore[x][y] ) {
                    list  [x][y] = ASL_OPEN;
                    gscore[x][y] = newg;
                    parent[x][y] = cur;
                    score [x][y] = gscore[x][y] + 2 * rl_dist(x, y, Tx, Ty);
                    open.push( std::make_pair( score[x][y], point(x, y) ) );
                }
            }
        }
    } while( !done && !open.empty() );

    std::vector<point> ret;
    if( done ) {
        point cur( Tx, Ty );
        while (cur.x != Fx || cur.y != Fy) {
            //debugmsg("Retracing... (%d:%d) => [%d:%d] => (%d:%d)", Tx, Ty, cur.x, cur.y, Fx, Fy);
            ret.push_back(cur);
            if( rl_dist( cur, parent[cur.x][cur.y] ) > 1 ){
                debugmsg("Jump in our route! %d:%d->%d:%d", cur.x, cur.y,
                            parent[cur.x][cur.y].x, parent[cur.x][cur.y].y);
                return ret;
            }
            cur = parent[cur.x][cur.y];
        }

        std::reverse( ret.begin(), ret.end() );
    }

    return ret;
}

int map::coord_to_angle ( const int x, const int y, const int tgtx, const int tgty ) const
{
    const double DBLRAD2DEG = 57.2957795130823f;
    //const double PI = 3.14159265358979f;
    const double DBLPI = 6.28318530717958f;
    double rad = atan2 ( static_cast<double>(tgty - y), static_cast<double>(tgtx - x) );
    if ( rad < 0 ) {
        rad = DBLPI - (0 - rad);
    }

    return int( rad * DBLRAD2DEG );
}

void map::save()
{
    for( int gridx = 0; gridx < my_MAPSIZE; gridx++ ) {
        for( int gridy = 0; gridy < my_MAPSIZE; gridy++ ) {
#ifdef ZLEVELS
            for( int gridz = -OVERMAP_DEPTH; gridz <= OVERMAP_HEIGHT; gridz++ ) {
                saven( gridx, gridy, gridz );
            }
#else
            saven( gridx, gridy, abs_sub.z );
#endif
        }
    }
}

void map::load(const int wx, const int wy, const int wz, const bool update_vehicle)
{
    for( auto & traps : traplocs ) {
        traps.clear();
    }
    set_abs_sub( wx, wy, wz );
    for (int gridx = 0; gridx < my_MAPSIZE; gridx++) {
        for (int gridy = 0; gridy < my_MAPSIZE; gridy++) {
            loadn( gridx, gridy, update_vehicle );
        }
    }
}

void map::shift_traps( const tripoint &shift )
{
    // Offset needs to have sign opposite to shift direction
    const tripoint offset( -shift.x * SEEX, -shift.y * SEEY, -shift.z );
    for( auto & traps : traplocs ) {
        for( auto iter = traps.begin(); iter != traps.end(); ) {
            tripoint &pos = *iter;
            pos += offset;
            if( inbounds( pos ) ) {
                ++iter;
            } else {
                // Theoretical enhancement: if this is not the last entry of the vector,
                // move the last entry into pos and remove the last entry instead of iter.
                // This would avoid moving all the remaining entries.
                iter = traps.erase( iter );
            }
        }
    }
}

void map::shift( const int sx, const int sy )
{
// Special case of 0-shift; refresh the map
    if( sx == 0 && sy == 0 ) {
        return; // Skip this?
    }
    const int absx = get_abs_sub().x;
    const int absy = get_abs_sub().y;
    const int wz = get_abs_sub().z;

    set_abs_sub( absx + sx, absy + sy, wz );

// if player is in vehicle, (s)he must be shifted with vehicle too
    if( g->u.in_vehicle ) {
        g->u.setx( g->u.posx() - sx * SEEX );
        g->u.sety( g->u.posy() - sy * SEEY );
    }

    shift_traps( tripoint( sx, sy, 0 ) );

    for( vehicle *veh : vehicle_list ) {
        veh->smx += sx;
        veh->smy += sy;
    }

// Clear vehicle list and rebuild after shift
    vehicle *remoteveh = g->remoteveh();
    clear_vehicle_cache();
    vehicle_list.clear();
// Shift the map sx submaps to the right and sy submaps down.
// sx and sy should never be bigger than +/-1.
// absx and absy are our position in the world, for saving/loading purposes.
    if (sx >= 0) {
        for (int gridx = 0; gridx < my_MAPSIZE; gridx++) {
            if (sy >= 0) {
                for (int gridy = 0; gridy < my_MAPSIZE; gridy++) {
                    if (gridx + sx < my_MAPSIZE && gridy + sy < my_MAPSIZE) {
                        copy_grid( point( gridx, gridy ),
                                   point( gridx + sx, gridy + sy ) );
                        update_vehicle_list(get_submap_at_grid(gridx, gridy));
                    } else {
                        loadn( gridx, gridy, true );
                    }
                }
            } else { // sy < 0; work through it backwards
                for (int gridy = my_MAPSIZE - 1; gridy >= 0; gridy--) {
                    if (gridx + sx < my_MAPSIZE && gridy + sy >= 0) {
                        copy_grid( point( gridx, gridy ),
                                   point( gridx + sx, gridy + sy ) );
                        update_vehicle_list(get_submap_at_grid(gridx, gridy));
                    } else {
                        loadn( gridx, gridy, true );
                    }
                }
            }
        }
    } else { // sx < 0; work through it backwards
        for (int gridx = my_MAPSIZE - 1; gridx >= 0; gridx--) {
            if (sy >= 0) {
                for (int gridy = 0; gridy < my_MAPSIZE; gridy++) {
                    if (gridx + sx >= 0 && gridy + sy < my_MAPSIZE) {
                        copy_grid( point( gridx, gridy ),
                                   point( gridx + sx, gridy + sy ) );
                        update_vehicle_list(get_submap_at_grid(gridx, gridy));
                    } else {
                        loadn( gridx, gridy, true );
                    }
                }
            } else { // sy < 0; work through it backwards
                for (int gridy = my_MAPSIZE - 1; gridy >= 0; gridy--) {
                    if (gridx + sx >= 0 && gridy + sy >= 0) {
                        copy_grid( point( gridx, gridy ),
                                   point( gridx + sx, gridy + sy ) );
                        update_vehicle_list(get_submap_at_grid(gridx, gridy));
                    } else {
                        loadn( gridx, gridy, true );
                    }
                }
            }
        }
    }
    reset_vehicle_cache();
    g->setremoteveh( remoteveh );
}

void map::vertical_shift( const int newz )
{
#ifndef ZLEVELS
    (void)newz;
    debugmsg( "Called map::vertical_shift outside z-level build (this shouldn't happen)" );
    return;
#else
    if( newz < -OVERMAP_DEPTH || newz > OVERMAP_HEIGHT ) {
        debugmsg( "Tried to get z-level %d outside allowed range of %d-%d", 
                  newz, -OVERMAP_DEPTH, OVERMAP_HEIGHT );
        return;
    }

    clear_vehicle_cache();
    vehicle_list.clear();
    set_transparency_cache_dirty();
    set_outside_cache_dirty();

    // Forgetting done, now get the new z-level
    tripoint trp = get_abs_sub();
    set_abs_sub( trp.x, trp.y, newz );

    for( int gridx = 0; gridx < my_MAPSIZE; gridx++ ) {
        for( int gridy = 0; gridy < my_MAPSIZE; gridy++ ) {
            update_vehicle_list( get_submap_at_grid( gridx, gridy, newz ) );
        }
    }

    reset_vehicle_cache();
#endif
}

// saven saves a single nonant.  worldx and worldy are used for the file
// name and specifies where in the world this nonant is.  gridx and gridy are
// the offset from the top left nonant:
// 0,0 1,0 2,0
// 0,1 1,1 2,1
// 0,2 1,2 2,2
// (worldx,worldy,worldz) denotes the absolute coordinate of the submap
// in grid[0].
void map::saven( const int gridx, const int gridy, const int gridz )
{
    dbg( D_INFO ) << "map::saven(worldx[" << abs_sub.x << "], worldy[" << abs_sub.y << "], worldz[" << abs_sub.z
                  << "], gridx[" << gridx << "], gridy[" << gridy << "], gridz[" << gridz << "])";
    const int gridn = get_nonant( gridx, gridy, gridz );
    submap *submap_to_save = getsubmap( gridn );
    if( submap_to_save == nullptr || submap_to_save->get_ter( 0, 0 ) == t_null ) {
        // This is a serious error and should be signaled as soon as possible
        debugmsg( "map::saven grid (%d,%d,%d) %s!", gridx, gridy, gridz,
                  submap_to_save == nullptr ? "null" : "uninitialized" );
        return;
    }

    const int abs_x = abs_sub.x + gridx;
    const int abs_y = abs_sub.y + gridy;
    const int abs_z = gridz;
#ifndef ZLEVELS
    if( gridz != abs_sub.z ) {
        debugmsg( "Tried to save submap (%d,%d,%d) as (%d,%d,%d), which isn't supported in non-z-level builds", 
                  abs_x, abs_y, abs_sub.z, abs_x, abs_y, gridz );
    }
#endif
    dbg( D_INFO ) << "map::saven abs_x: " << abs_x << "  abs_y: " << abs_y << "  abs_z: " << abs_z
                  << "  gridn: " << gridn;
    submap_to_save->turn_last_touched = int(calendar::turn);
    MAPBUFFER.add_submap( abs_x, abs_y, abs_z, submap_to_save );
}

// worldx & worldy specify where in the world this is;
// gridx & gridy specify which nonant:
// 0,0  1,0  2,0
// 0,1  1,1  2,1
// 0,2  1,2  2,2 etc
// (worldx,worldy,worldz) denotes the absolute coordinate of the submap
// in grid[0].
void map::loadn( const int gridx, const int gridy, const bool update_vehicles ) {
#ifdef ZLEVELS
    for( int gridz = -OVERMAP_DEPTH; gridz <= OVERMAP_HEIGHT; gridz++ ) {
        bool need_veh_update = update_vehicles && gridz == abs_sub.z;
#else
    int gridz = abs_sub.z;
    bool need_veh_update = update_vehicles;
    {
#endif
        // TODO: Update vehicles on all z-levels, but only after the veh cache becomes 3D
        
        loadn( gridx, gridy, gridz, need_veh_update );
    }
}

// Optimized mapgen function that only works properly for very simple overmap types
// Does not create or require a temporary map and does its own saving
static void generate_uniform( const int x, const int y, const int z, const oter_id &terrain_type )
{
    static const oter_id rock("empty_rock");
    static const oter_id air("open_air");

    dbg( D_INFO ) << "generate_uniform x: " << x << "  y: " << y << "  abs_z: " << z
                  << "  terrain_type: " << static_cast<std::string const&>(terrain_type);

    ter_id fill = t_null;
    if( terrain_type == rock ) {
        fill = t_rock;
    } else if( terrain_type == air ) {
        fill = t_open_air;
    } else {
        debugmsg( "map::generate_uniform called on non-uniform type: %s",
                  static_cast<std::string const&>(terrain_type).c_str() );
        return;
    }

    constexpr size_t block_size = SEEX * SEEY;
    for( int xd = 0; xd <= 1; xd++ ) {
        for( int yd = 0; yd <= 1; yd++ ) {
            submap *sm = new submap();
            sm->is_uniform = true;
            std::uninitialized_fill_n( &sm->ter[0][0], block_size, fill );
            sm->turn_last_touched = int(calendar::turn);
            MAPBUFFER.add_submap( x + xd, y + yd, z, sm );
        }
    }
}

void map::loadn( const int gridx, const int gridy, const int gridz, const bool update_vehicles )
{
    // Cache empty overmap types
    static const oter_id rock("empty_rock");
    static const oter_id air("open_air");

    dbg(D_INFO) << "map::loadn(game[" << g << "], worldx[" << abs_sub.x << "], worldy[" << abs_sub.y << "], gridx["
                << gridx << "], gridy[" << gridy << "], gridz[" << gridz << "])";

    const int absx = abs_sub.x + gridx,
              absy = abs_sub.y + gridy;
    const size_t gridn = get_nonant( gridx, gridy, gridz );

    dbg(D_INFO) << "map::loadn absx: " << absx << "  absy: " << absy
                << "  gridn: " << gridn;

    const int old_abs_z = abs_sub.z; // Ugly, but necessary at the moment
    abs_sub.z = gridz;

    submap *tmpsub = MAPBUFFER.lookup_submap(absx, absy, gridz);
    if( tmpsub == nullptr ) {
        // It doesn't exist; we must generate it!
        dbg( D_INFO | D_WARNING ) << "map::loadn: Missing mapbuffer data. Regenerating.";

        // Each overmap square is two nonants; to prevent overlap, generate only at
        //  squares divisible by 2.
        const int newmapx = absx - ( abs( absx ) % 2 );
        const int newmapy = absy - ( abs( absy ) % 2 );
        // Short-circuit if the map tile is uniform
        int overx = newmapx;
        int overy = newmapy;
        overmapbuffer::sm_to_omt( overx, overy );
        oter_id terrain_type = overmap_buffer.ter( overx, overy, gridz );
        if( terrain_type == rock || terrain_type == air ) {
            generate_uniform( newmapx, newmapy, gridz, terrain_type );
        } else {
            tinymap tmp_map;
            tmp_map.generate( newmapx, newmapy, gridz, calendar::turn );
        }

        // This is the same call to MAPBUFFER as above!
        tmpsub = MAPBUFFER.lookup_submap( absx, absy, gridz );
        if( tmpsub == nullptr ) {
            dbg( D_ERROR ) << "failed to generate a submap at " << absx << absy << abs_sub.z;
            debugmsg( "failed to generate a submap at %d,%d,%d", absx, absy, abs_sub.z );
            return;
        }
    }

    // New submap changes the content of the map and all caches must be recalculated
    set_transparency_cache_dirty();
    set_outside_cache_dirty();
    setsubmap( gridn, tmpsub );

    // Update vehicle data
    if( update_vehicles ) {
        for( auto it : tmpsub->vehicles ) {
            // Only add if not tracking already.
            if( vehicle_list.find( it ) == vehicle_list.end() ) {
                // gridx/y not correct. TODO: Fix
                it->smx = gridx;
                it->smy = gridy;
                vehicle_list.insert( it );
                update_vehicle_cache( it );
            }
        }
    }

    actualize( gridx, gridy, gridz );

    abs_sub.z = old_abs_z;
}

bool map::has_rotten_away( item &itm, const tripoint &pnt ) const
{
    if( itm.is_corpse() ) {
        itm.calc_rot( pnt );
        return itm.get_rot() > DAYS( 10 ) && !itm.can_revive();
    } else if( itm.goes_bad() ) {
        itm.calc_rot( pnt );
        return itm.has_rotten_away();
    } else if( itm.type->container && itm.type->container->preserves ) {
        // Containers like tin cans preserves all items inside, they do not rot at all.
        return false;
    } else if( itm.type->container && itm.type->container->seals ) {
        // Items inside rot but do not vanish as the container seals them in.
        for( auto &c : itm.contents ) {
            c.calc_rot( pnt );
        }
        return false;
    } else {
        // Check and remove rotten contents, but always keep the container.
        for( auto it = itm.contents.begin(); it != itm.contents.end(); ) {
            if( has_rotten_away( *it, pnt ) ) {
                it = itm.contents.erase( it );
            } else {
                ++it;
            }
        }

        return false;
    }
}

template <typename Container>
void map::remove_rotten_items( Container &items, const tripoint &pnt )
{
    const tripoint abs_pnt = getabs( pnt );
    for( auto it = items.begin(); it != items.end(); ) {
        if( has_rotten_away( *it, abs_pnt ) ) {
            it = i_rem( pnt, it );
        } else {
            ++it;
        }
    }
}

void map::fill_funnels( const tripoint &p )
{
    const auto &tr = tr_at( p );
    if( !tr.is_funnel() ) {
        return;
    }
    // Note: the inside/outside cache might not be correct at this time
    if( has_flag_ter_or_furn( TFLAG_INDOORS, p ) ) {
        return;
    }
    auto items = i_at( p );
    int maxvolume = 0;
    auto biggest_container = items.end();
    for( auto candidate = items.begin(); candidate != items.end(); ++candidate ) {
        if( candidate->is_funnel_container( maxvolume ) ) {
            biggest_container = candidate;
        }
    }
    if( biggest_container != items.end() ) {

        retroactively_fill_from_funnel( *biggest_container, tr, calendar::turn, getabs( p ) );
    }
}

void map::grow_plant( const tripoint &p )
{
    const auto &furn = furn_at( p );
    if( !furn.has_flag( "PLANT" ) ) {
        return;
    }
    auto items = i_at( p );
    if( items.empty() ) {
        // No seed there anymore, we don't know what kind of plant it was.
        dbg( D_ERROR ) << "a seed item has vanished at " << p.x << "," << p.y << "," << p.z;
        furn_set( p, f_null );
        return;
    }

    // Erase fertilizer tokens, but keep the seed item
    i_rem( p, 1 );
    auto seed = items.front();
    if( !seed.is_seed() ) {
        // No seed there anymore, we don't know what kind of plant it was.
        dbg( D_ERROR ) << "a planted item at " << p.x << "," << p.y << "," << p.z << " has no seed data";
        furn_set( p, f_null );
        return;
    }
    const int plantEpoch = seed.get_plant_epoch();

    if ( calendar::turn >= seed.bday + plantEpoch ) {
        if (calendar::turn < seed.bday + plantEpoch * 2 ) {
                furn_set(p, "f_plant_seedling");
        } else if (calendar::turn < seed.bday + plantEpoch * 3 ) {
                furn_set(p, "f_plant_mature");
        } else {
                furn_set(p, "f_plant_harvest");
        }
    }
}

void map::restock_fruits( const tripoint &p, int time_since_last_actualize )
{
    const auto &ter = ter_at( p );
    //if the fruit-bearing season of the already harvested terrain has passed, make it harvestable again
    if( !ter.has_flag( TFLAG_HARVESTED ) ) {
        return;
    }
    if( ter.harvest_season != calendar::turn.get_season() ||
        time_since_last_actualize >= DAYS( calendar::season_length() ) ) {
        ter_set( p, ter.transforms_into );
    }
}

void map::actualize( const int gridx, const int gridy, const int gridz )
{
    submap *const tmpsub = get_submap_at_grid( gridx, gridy, gridz );
    if( tmpsub == nullptr ) {
        debugmsg( "Actualize called on null submap (%d,%d,%d)", gridx, gridy, gridz );
        return;
    }

    const auto time_since_last_actualize = calendar::turn - tmpsub->turn_last_touched;
    const bool do_funnels = ( gridz >= 0 );

    // check spoiled stuff, and fill up funnels while we're at it
    for( int x = 0; x < SEEX; x++ ) {
        for( int y = 0; y < SEEY; y++ ) {
            const tripoint pnt( gridx * SEEX + x, gridy * SEEY + y, gridz );

            const auto &furn = furn_at( pnt );
            // plants contain a seed item which must not be removed under any circumstances
            if( !furn.has_flag( "PLANT" ) ) {
                remove_rotten_items( tmpsub->itm[x][y], pnt );
            }

            const auto trap_here = tmpsub->get_trap( x, y );
            if( trap_here != tr_null ) {
                traplocs[trap_here].push_back( pnt );
            }

            if( do_funnels ) {
                fill_funnels( pnt );
            }

            grow_plant( pnt );

            restock_fruits( pnt, time_since_last_actualize );
        }
    }

    //Merchants will restock their inventories every three days
    const int merchantRestock = 14400 * 3; //14400 is the length of one day
    //Check for Merchants to restock
    for( auto & i : g->active_npc ) {
        if( i->restock != -1 && calendar::turn > ( i->restock + merchantRestock ) ) {
            i->shop_restock();
            i->restock = int( calendar::turn );
        }
    }

    // the last time we touched the submap, is right now.
    tmpsub->turn_last_touched = calendar::turn;
}

void map::copy_grid( const point to, const point from )
{
#ifdef ZLEVELS
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
#else
    int z = abs_sub.z;
    {
#endif
        const auto smap = get_submap_at_grid( from.x, from.y, z );
        setsubmap( get_nonant( to.x, to.y, z ), smap );
        for( auto &it : smap->vehicles ) {
            it->smx = to.x;
            it->smy = to.y;
        }
    }
}

void map::spawn_monsters( const tripoint &gp, mongroup &group, bool ignore_sight )
{
    // TODO: Z
    const int gx = gp.x;
    const int gy = gp.y;
    const int s_range = std::min(SEEX * (MAPSIZE / 2), g->u.sight_range( g->light_level() ) );
    int pop = group.population;
    std::vector<point> locations;
    if( !ignore_sight ) {
        // If the submap is one of the outermost submaps, assume that monsters are
        // invisible there.
        // When the map shifts because of the player moving (called from game::plmove),
        // the player has still their *old* (not shifted) coordinates.
        // That makes the submaps that have come into view visible (if the sight range
        // is big enough).
        if( gx == 0 || gy == 0 || gx + 1 == MAPSIZE || gy + 1 == MAPSIZE ) {
            ignore_sight = true;
        }
    }
    for( int x = 0; x < SEEX; ++x ) {
        for( int y = 0; y < SEEY; ++y ) {
            int fx = x + SEEX * gx;
            int fy = y + SEEY * gy;
            if( g->critter_at( fx, fy ) != nullptr ) {
                continue; // there is already some creature
            }
            if( move_cost( fx, fy ) == 0 ) {
                continue; // solid area, impassable
            }
            int t;
            if( !ignore_sight && sees( g->u.posx(), g->u.posy(), fx, fy, s_range, t ) ) {
                continue; // monster must spawn outside the viewing range of the player
            }
            if( has_flag_ter_or_furn( TFLAG_INDOORS, fx, fy ) ) {
                continue; // monster must spawn outside.
            }
            locations.push_back( point( fx, fy ) );
        }
    }
    if( locations.empty() ) {
        // TODO: what now? there is now possible place to spawn monsters, most
        // likely because the player can see all the places.
        dbg( D_ERROR ) << "Empty locations for group " << group.type << " at " << gx << "," << gy;
        return;
    }
    for( int m = 0; m < pop; m++ ) {
        MonsterGroupResult spawn_details = MonsterGroupManager::GetResultFromGroup( group.type, &pop );
        if( spawn_details.name == "mon_null" ) {
            continue;
        }
        monster tmp( GetMType( spawn_details.name ) );
        for( int i = 0; i < spawn_details.pack_size; i++) {
            for( int tries = 0; tries < 10 && !locations.empty(); tries++ ) {
                const size_t index = rng( 0, locations.size() - 1 );
                const point p = locations[index];
                if( !tmp.can_move_to( p.x, p.y ) ) {
                    continue; // target can not contain the monster
                }
                tmp.spawn( p.x, p.y );
                g->add_zombie( tmp );
                locations.erase( locations.begin() + index );
                break;
            }
        }
    }
    // indicates the group is empty, and can be removed later
    group.population = 0;
}

void map::spawn_monsters(bool ignore_sight)
{
    for (int gx = 0; gx < my_MAPSIZE; gx++) {
        for (int gy = 0; gy < my_MAPSIZE; gy++) {
            auto groups = overmap_buffer.groups_at( abs_sub.x + gx, abs_sub.y + gy, abs_sub.z );
            for( auto &mgp : groups ) {
                // TODO: Z
                spawn_monsters( tripoint( gx, gy, abs_sub.z ), *mgp, ignore_sight );
            }

            submap * const current_submap = get_submap_at_grid(gx, gy);
            for (auto &i : current_submap->spawns) {
                for (int j = 0; j < i.count; j++) {
                    int tries = 0;
                    int mx = i.posx, my = i.posy;
                    monster tmp(GetMType(i.type));
                    tmp.mission_id = i.mission_id;
                    if (i.name != "NONE") {
                        tmp.unique_name = i.name;
                    }
                    if (i.friendly) {
                        tmp.friendly = -1;
                    }
                    int fx = mx + gx * SEEX, fy = my + gy * SEEY;

                    while ((!g->is_empty(fx, fy) || !tmp.can_move_to(fx, fy)) && tries < 10) {
                        mx = (i.posx + rng(-3, 3)) % SEEX;
                        my = (i.posy + rng(-3, 3)) % SEEY;
                        if (mx < 0) {
                            mx += SEEX;
                        }
                        if (my < 0) {
                            my += SEEY;
                        }
                        fx = mx + gx * SEEX;
                        fy = my + gy * SEEY;
                        tries++;
                    }
                    if (tries != 10) {
                        tmp.spawn(fx, fy);
                        g->add_zombie(tmp);
                    }
                }
            }
            current_submap->spawns.clear();
            overmap_buffer.spawn_monster( abs_sub.x + gx, abs_sub.y + gy, abs_sub.z );
        }
    }
}

void map::clear_spawns()
{
    for( auto & smap : grid ) {
        smap->spawns.clear();
    }
}

void map::clear_traps()
{
    for( auto & smap : grid ) {
        for (int x = 0; x < SEEX; x++) {
            for (int y = 0; y < SEEY; y++) {
                smap->set_trap(x, y, tr_null);
            }
        }
    }

    // Forget about all trap locations.
    for( auto &i : traplocs ) {
        i.clear();
    }
}

const std::vector<tripoint> &map::trap_locations(trap_id t) const
{
    return traplocs[t];
}

bool map::inbounds(const int x, const int y) const
{
    return (x >= 0 && x < SEEX * my_MAPSIZE && y >= 0 && y < SEEY * my_MAPSIZE);
}

bool map::inbounds(const int x, const int y, const int z) const
{
#ifdef ZLEVELS
    return (x >= 0 && x < SEEX * my_MAPSIZE && 
            y >= 0 && y < SEEY * my_MAPSIZE && 
            z >= -OVERMAP_DEPTH && z <= OVERMAP_HEIGHT);
#else
    (void)z;
    return (x >= 0 && x < SEEX * my_MAPSIZE && y >= 0 && y < SEEY * my_MAPSIZE);
#endif
}

bool map::inbounds( const tripoint &p ) const
{
 return (p.x >= 0 && p.x < SEEX * my_MAPSIZE && 
         p.y >= 0 && p.y < SEEY * my_MAPSIZE && 
         p.z >= -OVERMAP_DEPTH && p.z <= OVERMAP_HEIGHT);
}

void map::set_graffiti( const tripoint &p, const std::string &contents )
{
    if( !inbounds( p ) ) {
        return;
    }
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    current_submap->set_graffiti( lx, ly, contents );
}

void map::delete_graffiti( const tripoint &p )
{
    if( !inbounds( p ) ) {
        return;
    }
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    current_submap->delete_graffiti( lx, ly );
}

const std::string &map::graffiti_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        static const std::string empty_string;
        return empty_string;
    }
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    return current_submap->get_graffiti( lx, ly );
}

bool map::has_graffiti_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return false;
    }
    int lx, ly;
    submap *const current_submap = get_submap_at( p, lx, ly );
    return current_submap->has_graffiti( lx, ly );
}

long map::determine_wall_corner(const int x, const int y) const
{
    const bool above_connects = has_flag_ter( TFLAG_CONNECT_TO_WALL, x, y - 1 );
    const bool below_connects = has_flag_ter( TFLAG_CONNECT_TO_WALL, x, y + 1 );
    const bool left_connects  = has_flag_ter( TFLAG_CONNECT_TO_WALL, x - 1, y );
    const bool right_connects = has_flag_ter( TFLAG_CONNECT_TO_WALL, x + 1, y );
    const auto bits = ( above_connects ? 1 : 0 ) +
                      ( right_connects ? 2 : 0 ) +
                      ( below_connects ? 4 : 0 ) +
                      ( left_connects  ? 8 : 0 );
    switch( bits ) {
        case 1 | 2 | 4 | 8: return LINE_XXXX;
        case 0 | 2 | 4 | 8: return LINE_OXXX;
        
        case 1 | 0 | 4 | 8: return LINE_XOXX;
        case 0 | 0 | 4 | 8: return LINE_OOXX;

        case 1 | 2 | 0 | 8: return LINE_XXOX;
        case 0 | 2 | 0 | 8: return LINE_OXOX;
        case 1 | 0 | 0 | 8: return LINE_XOOX;
        case 0 | 0 | 0 | 8: return LINE_OXOX; // LINE_OOOX would be better

        case 1 | 2 | 4 | 0: return LINE_XXXO;
        case 0 | 2 | 4 | 0: return LINE_OXXO;
        case 1 | 0 | 4 | 0: return LINE_XOXO;
        case 0 | 0 | 4 | 0: return LINE_XOXO; // LINE_OOXO would be better
        case 1 | 2 | 0 | 0: return LINE_XXOO;
        case 0 | 2 | 0 | 0: return LINE_OXOX; // LINE_OXOO would be better
        case 1 | 0 | 0 | 0: return LINE_XOXO; // LINE_XOOO would be better

        case 0 | 0 | 0 | 0: return ter_at( x, y ).sym; // technically just a column

        default:
            // assert( false );
            // this shall not happen
            return '?';
    }
}

void map::build_outside_cache()
{
    if (!outside_cache_dirty) {
        return;
    }

    if (abs_sub.z < 0)
    {
        memset(outside_cache, false, sizeof(outside_cache));
        return;
    }
    memset(outside_cache, true, sizeof(outside_cache));

    for(int x = 0; x < SEEX * my_MAPSIZE; x++)
    {
        for(int y = 0; y < SEEY * my_MAPSIZE; y++)
        {
            if( has_flag_ter_or_furn(TFLAG_INDOORS, x, y))
            {
                for( int dx = -1; dx <= 1; dx++ )
                {
                    for( int dy = -1; dy <= 1; dy++ )
                    {
                        if(INBOUNDS(x + dx, y + dy))
                        {
                            outside_cache[x + dx][y + dy] = false;
                        }
                    }
                }
            }
        }
    }

    outside_cache_dirty = false;
}

void map::build_map_cache()
{
    build_outside_cache();

    build_transparency_cache();

    // Cache all the vehicle stuff in one loop
    VehicleList vehs = get_vehicles();
    for(auto &v : vehs) {
        for (size_t part = 0; part < v.v->parts.size(); part++) {
            int px = v.x + v.v->parts[part].precalc[0].x;
            int py = v.y + v.v->parts[part].precalc[0].y;
            if(INBOUNDS(px, py)) {
                if (v.v->is_inside(part)) {
                    outside_cache[px][py] = false;
                }
                if (v.v->part_flag(part, VPFLAG_OPAQUE) && v.v->parts[part].hp > 0) {
                    int dpart = v.v->part_with_feature(part , VPFLAG_OPENABLE);
                    if (dpart < 0 || !v.v->parts[dpart].open) {
                        transparency_cache[px][py] = LIGHT_TRANSPARENCY_SOLID;
                    }
                }
            }
        }
    }

    build_seen_cache();
    generate_lightmap();
}

std::vector<point> closest_points_first(int radius, point p)
{
    return closest_points_first(radius, p.x, p.y);
}

//this returns points in a spiral pattern starting at center_x/center_y until it hits the radius. clockwise fashion
//credit to Tom J Nowell; http://stackoverflow.com/a/1555236/1269969
std::vector<point> closest_points_first(int radius, int center_x, int center_y)
{
    std::vector<point> points;
    int X,Y,x,y,dx,dy;
    X = Y = (radius * 2) + 1;
    x = y = dx = 0;
    dy = -1;
    int t = std::max(X,Y);
    int maxI = t * t;
    for(int i = 0; i < maxI; i++)
    {
        if ((-X/2 <= x) && (x <= X/2) && (-Y/2 <= y) && (y <= Y/2))
        {
            points.push_back(point(x + center_x, y + center_y));
        }
        if( (x == y) || ((x < 0) && (x == -y)) || ((x > 0) && (x == 1 - y)))
        {
            t = dx;
            dx = -dy;
            dy = t;
        }
        x += dx;
        y += dy;
    }
    return points;
}

std::vector<tripoint> closest_tripoints_first( int radius, const tripoint &center )
{
    std::vector<tripoint> points;
    int X,Y,x,y,dx,dy;
    X = Y = (radius * 2) + 1;
    x = y = dx = 0;
    dy = -1;
    int t = std::max(X,Y);
    int maxI = t * t;
    for(int i = 0; i < maxI; i++)
    {
        if ((-X/2 <= x) && (x <= X/2) && (-Y/2 <= y) && (y <= Y/2))
        {
            points.push_back( tripoint( x + center.x, y + center.y, center.z ) );
        }
        if( (x == y) || ((x < 0) && (x == -y)) || ((x > 0) && (x == 1 - y)))
        {
            t = dx;
            dx = -dy;
            dy = t;
        }
        x += dx;
        y += dy;
    }
    return points;
}
//////////
///// coordinate helpers

point map::getabs(const int x, const int y) const
{
    return point( x + abs_sub.x * SEEX, y + abs_sub.y * SEEY );
}

tripoint map::getabs( const tripoint &p ) const
{
    return tripoint( p.x + abs_sub.x * SEEX, p.y + abs_sub.y * SEEY, p.z );
}

point map::getlocal(const int x, const int y) const {
    return point( x - abs_sub.x * SEEX, y - abs_sub.y * SEEY );
}

tripoint map::getlocal( const tripoint &p ) const
{
    return tripoint( p.x - abs_sub.x * SEEX, p.y - abs_sub.y * SEEY, p.z );
}

void map::set_abs_sub(const int x, const int y, const int z)
{
    abs_sub = tripoint( x, y, z );
}

tripoint map::get_abs_sub() const
{
   return abs_sub;
}

submap *map::getsubmap( const size_t grididx ) const
{
    if( grididx >= grid.size() ) {
        debugmsg( "Tried to access invalid grid index %d. Grid size: %d", grididx, grid.size() );
        return nullptr;
    }
    return grid[grididx];
}

void map::setsubmap( const size_t grididx, submap * const smap )
{
    if( grididx >= grid.size() ) {
        debugmsg( "Tried to access invalid grid index %d", grididx );
        return;
    } else if( smap == nullptr ) {
        debugmsg( "Tried to set NULL submap pointer at index %d", grididx );
        return;
    }
    grid[grididx] = smap;
}

submap *map::get_submap_at( const int x, const int y, const int z ) const
{
    if( !inbounds( x, y, z ) ) {
        debugmsg( "Tried to access invalid map position (%d,%d, %d)", x, y, z );
        return nullptr;
    }
    return get_submap_at_grid( x / SEEX, y / SEEY, z );
}

submap *map::get_submap_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        debugmsg( "Tried to access invalid map position (%d,%d, %d)", p.x, p.y, p.z );
        return nullptr;
    }
    return get_submap_at_grid( p.x / SEEX, p.y / SEEY, p.z );
}

submap *map::get_submap_at( const int x, const int y ) const
{
    return get_submap_at( x, y, abs_sub.z );
}

submap *map::get_submap_at( const int x, const int y, int &offset_x, int &offset_y ) const
{
    return get_submap_at( x, y, abs_sub.z, offset_x, offset_y );
}

submap *map::get_submap_at( const int x, const int y, const int z, int &offset_x, int &offset_y ) const
{
    offset_x = x % SEEX;
    offset_y = y % SEEY;
    return get_submap_at( x, y, z );
}

submap *map::get_submap_at( const tripoint &p, int &offset_x, int &offset_y ) const
{
    offset_x = p.x % SEEX;
    offset_y = p.y % SEEY;
    return get_submap_at( p );
}

submap *map::get_submap_at_grid( const int gridx, const int gridy ) const
{
    return getsubmap( get_nonant( gridx, gridy ) );
}

submap *map::get_submap_at_grid( const int gridx, const int gridy, const int gridz ) const
{
    return getsubmap( get_nonant( gridx, gridy, gridz ) );
}

submap *map::get_submap_at_grid( const tripoint &p ) const
{
    return getsubmap( get_nonant( p.x, p.y, p.z ) );
}

size_t map::get_nonant( const int gridx, const int gridy ) const
{
    return get_nonant( gridx, gridy, abs_sub.z );
}

size_t map::get_nonant( const int gridx, const int gridy, const int gridz ) const
{
#ifdef ZLEVELS
    if( gridx < 0 || gridx >= my_MAPSIZE ||
        gridy < 0 || gridy >= my_MAPSIZE ||
        gridz < -OVERMAP_DEPTH || gridz > OVERMAP_HEIGHT) {
        debugmsg( "Tried to access invalid map position at grid (%d,%d,%d)", gridx, gridy, gridz );
        return 0;
    }

    const int indexz = gridz + OVERMAP_HEIGHT; // Can't be lower than 0
    return indexz + ( gridx + gridy * my_MAPSIZE ) * OVERMAP_LAYERS;
#else
    if( gridx < 0 || gridx >= my_MAPSIZE ||
        gridy < 0 || gridy >= my_MAPSIZE ) {
        debugmsg( "Tried to access invalid map position at grid (%d,%d,%d)", gridx, gridy, gridz );
        return 0;
    }

    return gridx + gridy * my_MAPSIZE;
#endif
}

tinymap::tinymap(int mapsize)
: map(mapsize)
{
}

ter_id find_ter_id(const std::string id, bool complain=true) {
    (void)complain; //FIXME: complain unused
    if( termap.find(id) == termap.end() ) {
         debugmsg("Can't find termap[%s]",id.c_str());
         return 0;
    }
    return termap[id].loadid;
}

ter_id find_furn_id(const std::string id, bool complain=true) {
    (void)complain; //FIXME: complain unused
    if( furnmap.find(id) == furnmap.end() ) {
         debugmsg("Can't find furnmap[%s]",id.c_str());
         return 0;
    }
    return furnmap[id].loadid;
}
void map::draw_line_ter(const ter_id type, int x1, int y1, int x2, int y2)
{
    std::vector<point> line = line_to(x1, y1, x2, y2, 0);
    for (auto &i : line) {
        ter_set(i.x, i.y, type);
    }
    ter_set(x1, y1, type);
}
void map::draw_line_ter(const std::string type, int x1, int y1, int x2, int y2) {
    draw_line_ter(find_ter_id(type), x1, y1, x2, y2);
}


void map::draw_line_furn(furn_id type, int x1, int y1, int x2, int y2) {
    std::vector<point> line = line_to(x1, y1, x2, y2, 0);
    for (auto &i : line) {
        furn_set(i.x, i.y, type);
    }
    furn_set(x1, y1, type);
}
void map::draw_line_furn(const std::string type, int x1, int y1, int x2, int y2) {
    draw_line_furn(find_furn_id(type), x1, y1, x2, y2);
}

void map::draw_fill_background(ter_id type) {
    // Need to explicitly set caches dirty - set_ter would do it before
    set_transparency_cache_dirty();
    set_outside_cache_dirty();

    // Fill each submap rather than each tile
    constexpr size_t block_size = SEEX * SEEY;
    for( int gridx = 0; gridx < my_MAPSIZE; gridx++ ) {
        for( int gridy = 0; gridy < my_MAPSIZE; gridy++ ) {
            auto sm = get_submap_at_grid( gridx, gridy );
            sm->is_uniform = true;
            std::uninitialized_fill_n( &sm->ter[0][0], block_size, type );
        }
    }
}

void map::draw_fill_background(std::string type) {
    draw_fill_background( find_ter_id(type) );
}
void map::draw_fill_background(ter_id (*f)()) {
    draw_square_ter(f, 0, 0, SEEX * my_MAPSIZE - 1, SEEY * my_MAPSIZE - 1);
}
void map::draw_fill_background(const id_or_id & f) {
    draw_square_ter(f, 0, 0, SEEX * my_MAPSIZE - 1, SEEY * my_MAPSIZE - 1);
}

void map::draw_square_ter(ter_id type, int x1, int y1, int x2, int y2) {
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            ter_set(x, y, type);
        }
    }
}
void map::draw_square_ter(std::string type, int x1, int y1, int x2, int y2) {
    draw_square_ter(find_ter_id(type), x1, y1, x2, y2);
}

void map::draw_square_furn(furn_id type, int x1, int y1, int x2, int y2) {
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            furn_set(x, y, type);
        }
    }
}
void map::draw_square_furn(std::string type, int x1, int y1, int x2, int y2) {
    draw_square_furn(find_furn_id(type), x1, y1, x2, y2);
}

void map::draw_square_ter(ter_id (*f)(), int x1, int y1, int x2, int y2) {
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            ter_set(x, y, f());
        }
    }
}

void map::draw_square_ter(const id_or_id & f, int x1, int y1, int x2, int y2) {
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            ter_set(x, y, f.get());
        }
    }
}

void map::draw_rough_circle(ter_id type, int x, int y, int rad) {
    for (int i = x - rad; i <= x + rad; i++) {
        for (int j = y - rad; j <= y + rad; j++) {
            if (rl_dist(x, y, i, j) + rng(0, 3) <= rad) {
                ter_set(i, j, type);
            }
        }
    }
}
void map::draw_rough_circle(std::string type, int x, int y, int rad) {
    draw_rough_circle(find_ter_id(type), x, y, rad);
}

void map::draw_rough_circle_furn(furn_id type, int x, int y, int rad) {
    for (int i = x - rad; i <= x + rad; i++) {
        for (int j = y - rad; j <= y + rad; j++) {
            if (rl_dist(x, y, i, j) + rng(0, 3) <= rad) {
                furn_set(i, j, type);
            }
        }
    }
}
void map::draw_rough_circle_furn(std::string type, int x, int y, int rad) {
    draw_rough_circle(find_furn_id(type), x, y, rad);
}

void map::add_corpse( const tripoint &p ) {
    item body;

    const bool isReviveSpecial = one_in(10);

    if (!isReviveSpecial){
        body.make_corpse();
    } else {
        body.make_corpse( "mon_zombie", calendar::turn );
        body.item_tags.insert("REVIVE_SPECIAL");
        body.active = true;
    }

    add_item_or_charges(p, body);
    put_items_from_loc( "shoes",  p, 0);
    put_items_from_loc( "pants",  p, 0);
    put_items_from_loc( "shirts", p, 0);
    if (one_in(6)) {
        put_items_from_loc("jackets", p, 0);
    }
    if (one_in(15)) {
        put_items_from_loc("bags", p, 0);
    }
}

/**
 * Adds vehicles to the current submap, selected from a random weighted
 * distribution of possible vehicles. If the road has a pavement, then set the
 * 'city' flag to true to spawn wrecks. If it doesn't (ie, highway or country
 * road,) then set 'city' to false to spawn far fewer vehicles that are out
 * of gas instead of wrecked.
 * @param city Whether or not to spawn city wrecks.
 * @param facing The direction the spawned car should face (multiple of 90).
 */
void map::add_road_vehicles(bool city, int facing)
{
    if (city) {
        int spawn_type = rng(0, 100);
        if(spawn_type <= 33) {
            //Randomly-distributed wrecks
            int maxwrecks = rng(1, 3);
            for (int nv = 0; nv < maxwrecks; nv++) {
                int vx = rng(0, 19);
                int vy = rng(0, 19);
                int car_type = rng(1, 100);
                if (car_type <= 25) {
                    add_vehicle("car", vx, vy, facing, -1, 1);
                } else if (car_type <= 30) {
                    add_vehicle("policecar", vx, vy, facing, -1, 1);
                } else if (car_type <= 39) {
                    add_vehicle("ambulance", vx, vy, facing, -1, 1);
                } else if (car_type <= 40) {
                    add_vehicle("bicycle_electric", vx, vy, facing, -1, 1);
                } else if (car_type <= 45) {
                    add_vehicle("beetle", vx, vy, facing, -1, 1);
                } else if (car_type <= 48) {
                    add_vehicle("car_sports", vx, vy, facing, -1, 1);
                } else if (car_type <= 50) {
                    add_vehicle("scooter", vx, vy, facing, -1, 1);
                } else if (car_type <= 53) {
                    add_vehicle("scooter_electric", vx, vy, facing, -1, 1);
                } else if (car_type <= 55) {
                    add_vehicle("motorcycle", vx, vy, facing, -1, 1);
                } else if (car_type <= 65) {
                    add_vehicle("hippie_van", vx, vy, facing, -1, 1);
                } else if (car_type <= 70) {
                    add_vehicle("cube_van_cheap", vx, vy, facing, -1, 1);
                } else if (car_type <= 75) {
                    add_vehicle("cube_van", vx, vy, facing, -1, 1);
                } else if (car_type <= 80) {
                    add_vehicle("electric_car", vx, vy, facing, -1, 1);
                } else if (car_type <= 90) {
                    add_vehicle("flatbed_truck", vx, vy, facing, -1, 1);
                } else if (car_type <= 95) {
                    add_vehicle("rv", vx, vy, facing, -1, 1);
                } else if (car_type <= 96) {
                    add_vehicle("lux_rv", vx, vy, facing, -1, 1);
                } else if (car_type <= 98) {
                    add_vehicle("meth_lab", vx, vy, facing, -1, 1);
                } else if (car_type <= 99) {
                    add_vehicle("apc", vx, vy, facing, -1, 1);
                } else {
                    add_vehicle("motorcycle_sidecart", vx, vy, facing, -1, 1);
                }
            }
        } else if(spawn_type <= 66) {
            //Parked vehicles
            int veh_x = 0;
            int veh_y = 0;
            if(facing == 0) {
                veh_x = rng(4, 16);
                veh_y = 17;
            } else if(facing == 90) {
                veh_x = 6;
                veh_y = rng(4, 16);
            } else if(facing == 180) {
                veh_x = rng(4, 16);
                veh_y = 6;
            } else if(facing == 270) {
                veh_x = 17;
                veh_y = rng(4, 16);
            }
            int veh_type = rng(0, 100);
            if(veh_type <= 67) {
                add_vehicle("car", veh_x, veh_y, facing, -1, 1);
            } else if(veh_type <= 89) {
                add_vehicle("electric_car", veh_x, veh_y, facing, -1, 1);
            } else if(veh_type <= 92) {
                add_vehicle("road_roller", veh_x, veh_y, facing, -1, 1);
            } else if(veh_type <= 97) {
                add_vehicle("policecar", veh_x, veh_y, facing, -1, 1);
            } else {
                add_vehicle("autosweeper", veh_x, veh_y, facing, -1, 1);
            }
        } else if(spawn_type <= 99) {
            //Totally clear section of road
            return;
        } else {
            //Road-blocking obstacle of some kind.
            int block_type = rng(0, 100);
            if(block_type <= 75) {
                //Jack-knifed semi
                int semi_x = 0;
                int semi_y = 0;
                int trailer_x = 0;
                int trailer_y = 0;
                if(facing == 0) {
                    semi_x = rng(0, 16);
                    semi_y = rng(14, 16);
                    trailer_x = semi_x + 4;
                    trailer_y = semi_y - 10;
                } else if(facing == 90) {
                    semi_x = rng(0, 8);
                    semi_y = rng(4, 15);
                    trailer_x = semi_x + 12;
                    trailer_y = semi_y + 1;
                } else if(facing == 180) {
                    semi_x = rng(4, 16);
                    semi_y = rng(4, 6);
                    trailer_x = semi_x - 4;
                    trailer_y = semi_y + 10;
                } else {
                    semi_x = rng(12, 20);
                    semi_y = rng(5, 16);
                    trailer_x = semi_x - 12;
                    trailer_y = semi_y - 1;
                }
                add_vehicle("semi_truck", semi_x, semi_y, (facing + 135) % 360, -1, 1);
                add_vehicle("truck_trailer", trailer_x, trailer_y, (facing + 90) % 360, -1, 1);
            } else {
                //Huge pileup of random vehicles
                std::string next_vehicle;
                int num_cars = rng(18, 22);
                bool policecars = block_type >= 95; //Policecar pileup, Blues Brothers style
                vehicle *last_added_car = NULL;
                for(int i = 0; i < num_cars; i++) {
                    if(policecars) {
                        next_vehicle = "policecar";
                    } else {
                        //Random car
                        int car_type = rng(0, 100);
                        if(car_type <= 70) {
                            next_vehicle = "car";
                        } else if(car_type <= 90) {
                            next_vehicle = "pickup";
                        } else if(car_type <= 95) {
                            next_vehicle = "cube_van";
                        } else {
                            next_vehicle = "hippie_van";
                        }
                    }
                    last_added_car = add_vehicle(next_vehicle, rng(4, 16), rng(4, 16), rng(0, 3) * 90, -1, 1);
                }

                //Hopefully by the last one we've got a giant pileup, so name it
                if (last_added_car != NULL) {
                    if(policecars) {
                        last_added_car->name = _("policecar pile-up");
                    } else {
                        last_added_car->name = _("pile-up");
                    }
                }
            }
        }
    } else {
        // spawn regular road out of fuel vehicles
        if (one_in(40)) {
            int vx = rng(8, 16);
            int vy = rng(8, 16);
            int car_type = rng(1, 27);
            if (car_type <= 10) {
                add_vehicle("car", vx, vy, facing, 0, -1);
            } else if (car_type <= 14) {
                add_vehicle("car_sports", vx, vy, facing, 0, -1);
            } else if (car_type <= 16) {
                add_vehicle("pickup", vx, vy, facing, 0, -1);
            } else if (car_type <= 18) {
                add_vehicle("semi_truck", vx, vy, facing, 0, -1);
            } else if (car_type <= 20) {
                add_vehicle("humvee", vx, vy, facing, 0, -1);
            } else if (car_type <= 24) {
                add_vehicle("rara_x", vx, vy, facing, 0, -1);
            } else if (car_type <= 25) {
                add_vehicle("apc", vx, vy, facing, 0, -1);
            } else {
                add_vehicle("armored_car", vx, vy, facing, 0, -1);
            }
        }
    }
}

// 2D overloads for fields
const field &map::field_at( const int x, const int y ) const
{
    return field_at( tripoint( x, y, abs_sub.z ) );
}

int map::get_field_age( const point p, const field_id t ) const
{
    return get_field_age( tripoint( p, abs_sub.z ), t );
}

int map::get_field_strength( const point p, const field_id t ) const
{
    return get_field_strength( tripoint( p, abs_sub.z ), t );
}

int map::adjust_field_age( const point p, const field_id t, const int offset )
{
    return adjust_field_age( tripoint( p, abs_sub.z ), t, offset );
}

int map::adjust_field_strength( const point p, const field_id t, const int offset )
{
    return adjust_field_strength( tripoint( p, abs_sub.z ), t, offset );
}

int map::set_field_age( const point p, const field_id t, const int age, bool isoffset )
{
    return set_field_age( tripoint( p, abs_sub.z ), t, age, isoffset );
}

int map::set_field_strength( const point p, const field_id t, const int str, bool isoffset )
{
    return set_field_strength( tripoint( p, abs_sub.z ), t, str, isoffset );
}

field_entry *map::get_field( const point p, const field_id t )
{
    return get_field( tripoint( p, abs_sub.z ), t );
}

bool map::add_field(const point p, const field_id t, const int density, const int age)
{
    return add_field( tripoint( p, abs_sub.z ), t, density, age );
}

bool map::add_field(const int x, const int y, const field_id t, const int density)
{
    return add_field( tripoint( x, y, abs_sub.z ), t, density, 0 );
}

void map::remove_field( const int x, const int y, const field_id field_to_remove )
{
    remove_field( tripoint( x, y, abs_sub.z ), field_to_remove );
}

field &map::get_field( const int x, const int y )
{
    return field_at( tripoint( x, y, abs_sub.z ) );
}

void map::creature_on_trap( Creature &c, bool const may_avoid )
{
    auto const &tr = tr_at( c.pos3() );
    if( tr.is_null() ) {
        return;
    }
    // boarded in a vehicle means the player is above the trap, like a flying monster and can
    // never trigger the trap.
    const player * const p = dynamic_cast<const player *>( &c );
    if( p != nullptr && p->in_vehicle ) {
        return;
    }
    if( may_avoid && c.avoid_trap( c.pos3(), tr ) ) {
        return;
    }
    tr.trigger( c.pos3(), &c );
}

template<typename Functor>
    void map::function_over( const tripoint &start, const tripoint &end, Functor fun ) const
{
    function_over( start.x, start.y, start.z, end.x, end.y, end.z, fun );
}

template<typename Functor>
    void map::function_over( const int stx, const int sty, const int stz, 
                             const int enx, const int eny, const int enz, Functor fun ) const
{
    // start and end are just two points, end can be "before" start
    // Also clip the area to map area
    const int minx = std::max( std::min(stx, enx ), 0 );
    const int miny = std::max( std::min(sty, eny ), 0 );
    const int minz = std::max( std::min(stz, enz ), -OVERMAP_DEPTH );
    const int maxx = std::min( std::max(stx, enx ), my_MAPSIZE * SEEX );
    const int maxy = std::min( std::max(sty, eny ), my_MAPSIZE * SEEY );
    const int maxz = std::min( std::max(stz, enz ), OVERMAP_HEIGHT );

    // Submaps that contain the bounding points
    const int min_smx = minx / SEEX;
    const int min_smy = miny / SEEY;
    const int max_smx = ( maxx + SEEX - 1 ) / SEEX;
    const int max_smy = ( maxy + SEEY - 1 ) / SEEY;
    // Z outermost, because submaps are flat
    tripoint gp;
    int &z = gp.z;
    int &smx = gp.x;
    int &smy = gp.y;
    for( z = minz; z <= maxz; z++ ) {
        for( smx = min_smx; smx <= max_smx; smx++ ) {
            for( smy = min_smy; smy <= max_smy; smy++ ) {
                submap const *cur_submap = get_submap_at_grid( smx, smy, z );
                // Bounds on the submap coords
                const int sm_minx = smx > min_smx ? 0 : minx % SEEX;
                const int sm_miny = smy > min_smy ? 0 : miny % SEEY;
                const int sm_maxx = smx < max_smx ? (SEEX - 1) : maxx % SEEX;
                const int sm_maxy = smy < max_smy ? (SEEY - 1) : maxy % SEEY;

                point lp;
                int &sx = lp.x;
                int &sy = lp.y;
                for( sx = sm_minx; sx <= sm_maxx; ++sx ) {
                    for( sy = sm_miny; sy <= sm_maxy; ++sy ) {
                        const iteration_state rval = fun( gp, cur_submap, lp );
                        if( rval != ITER_CONTINUE ) {
                            switch( rval ) {
                            case ITER_SKIP_ZLEVEL:
                                smx = my_MAPSIZE + 1;
                                smy = my_MAPSIZE + 1;
                                // Fall through
                            case ITER_SKIP_SUBMAP:
                                sx = SEEX;
                                sy = SEEY;
                                break;
                            default:
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
}

void map::scent_blockers( bool blocks_scent[SEEX * MAPSIZE][SEEY * MAPSIZE],
                     bool reduces_scent[SEEX * MAPSIZE][SEEY * MAPSIZE] )
{
    auto reduce = TFLAG_REDUCE_SCENT;
    auto block = TFLAG_WALL;
    auto fill_values = [&]( const tripoint &gp, const submap *sm, const point &lp ) {
        if( terlist[ sm->get_ter( lp.x, lp.y ) ].has_flag( block ) ) {
            // We need to generate the x/y coords, because we can't get them "for free"
            const int x = ( gp.x * SEEX ) + lp.x;
            const int y = ( gp.y * SEEY ) + lp.y;
            blocks_scent[x][y] = true;
        } else if( terlist[ sm->get_ter( lp.x, lp.y ) ].has_flag( reduce ) || furnlist[ sm->get_furn( lp.x, lp.y ) ].has_flag( reduce ) ) {
            const int x = ( gp.x * SEEX ) + lp.x;
            const int y = ( gp.y * SEEY ) + lp.y;
            reduces_scent[x][y] = true;
        }

        return ITER_CONTINUE;
    };

    function_over( 0, 0, abs_sub.z, SEEX * MAPSIZE, SEEY * MAPSIZE, abs_sub.z, fill_values );

    // Now vehicles
    auto vehs = get_vehicles();
    for( auto &wrapped_veh : vehs ) {
        vehicle &veh = *(wrapped_veh.v);
        auto obstacles = veh.all_parts_with_feature( VPFLAG_OBSTACLE, true );
        for( const int p : obstacles ) {
            const point part_pos = veh.global_pos() + veh.parts[p].precalc[0];
            if( inbounds( part_pos.x, part_pos.y ) ) {
                reduces_scent[part_pos.x][part_pos.y] = true;
            }
        }

        // Doors, but only the closed ones
        auto doors = veh.all_parts_with_feature( VPFLAG_OPENABLE, true );
        for( const int p : doors ) {
            if( veh.parts[p].open ) {
                continue;
            }

            const point part_pos = veh.global_pos() + veh.parts[p].precalc[0];
            if( inbounds( part_pos.x, part_pos.y ) ) {
                reduces_scent[part_pos.x][part_pos.y] = true;
            }
        }
    }
}

void map::scent_slime( int grscent[SEEX * MAPSIZE][SEEY * MAPSIZE] )
{
    auto find_fields = [&]( const tripoint &gp, const submap *sm, const point &lp ) {
        const field_entry *fd = sm->fld[lp.x][lp.y].findField( fd_slime );
        if( fd != nullptr ) {
            // We need to generate the x/y coords, because we can't get them "for free"
            const int x = ( gp.x * SEEX ) + lp.x;
            const int y = ( gp.y * SEEY ) + lp.y;
            const int fslime = fd->getFieldDensity() * 10;
            if( grscent[x][y] < fslime ) {
                grscent[x][y] = fslime;
            }
        }

        return sm->field_count > 0 ? ITER_CONTINUE : ITER_SKIP_SUBMAP;
    };

    function_over( 0, 0, abs_sub.z, SEEX * MAPSIZE, SEEY * MAPSIZE, abs_sub.z, find_fields );
}
