#include <algorithm>
#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * Harkinian Buddy packets for buddies that move and fight in your game.
 *
 * BUDDY_ATTACK (app -> game): a buddy swings. Its dummy player gets a sword hitbox for a few
 * frames: enemies flinch and take damage, grass and pots break, like your swings.
 *
 * BUDDY_POS (game -> app): where the game's collision put a buddy it's walking (several times a
 * second while the app is steering it).
 *
 * ENEMY_STATE (game -> app): the enemies near a buddy - what they are, where, how much health -
 * so the app can pick a target and stop swinging at dead ones.
 */

void Anchor::HandlePacket_BuddyAttack(nlohmann::json payload) {
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (!clients.contains(clientId)) {
        return;
    }
    AnchorClient& client = clients[clientId];
    if (!client.isBuddy) {
        return;
    }
    std::string kind = payload.value("kind", std::string("slash"));
    u8 sword = payload.value("sword", (u8)1); // 1 Kokiri, 2 Master, 3 Biggoron, 4 stick, 5 hammer
    u32 flags = DMG_SLASH_KOKIRI;
    u8 damage = 1;
    switch (sword) {
        case 2:
            flags = DMG_SLASH_MASTER;
            damage = 2;
            break;
        case 3:
            flags = DMG_SLASH_GIANT;
            damage = 4;
            break;
        case 4:
            flags = DMG_DEKU_STICK;
            damage = 2;
            break;
        case 5:
            flags = DMG_HAMMER_SWING;
            damage = 2;
            break;
        default:
            break;
    }
    if (kind == "spin") {
        flags = sword == 3 ? DMG_SPIN_GIANT : sword == 2 ? DMG_SPIN_MASTER : DMG_SPIN_KOKIRI;
        damage *= 2;
    } else if (kind == "jump") {
        flags = sword == 3 ? DMG_JUMP_GIANT : sword == 2 ? DMG_JUMP_MASTER : DMG_JUMP_KOKIRI;
        damage *= 2;
    }
    client.attackDmgFlags = flags;
    client.attackDamage = damage;
    client.attackSpin = kind == "spin";
    client.attackFrames = (u8)std::clamp(payload.value("frames", 4), 1, 12);
}

void Anchor::SendPacket_BuddyPos(u32 clientId, Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = BUDDY_POS;
    payload["targetClientId"] = clientId;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["pos"] = actor->world.pos;
    payload["yaw"] = actor->shape.rot.y;
    payload["ground"] = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    payload["wall"] = (actor->bgCheckFlags & BGCHECKFLAG_WALL) != 0;
    payload["water"] = actor->yDistToWater;
    payload["floorY"] = actor->floorHeight;
    payload["velY"] = actor->velocity.y;
    payload["quiet"] = true;
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_EnemyState(u32 clientId, Vec3f center) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }
    nlohmann::json enemies = nlohmann::json::array();
    for (s32 cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->update == nullptr || (a->colChkInfo.health == 0 && cat != ACTORCAT_BOSS)) {
                continue;
            }
            f32 d = Math_Vec3f_DistXYZ(&a->world.pos, &center);
            if (d > 1200.0f) {
                continue;
            }
            nlohmann::json e;
            e["key"] = (u64)(uintptr_t)a & 0xFFFFFFFF;
            e["id"] = a->id;
            e["params"] = a->params;
            e["boss"] = cat == ACTORCAT_BOSS;
            e["pos"] = a->world.pos;
            e["health"] = a->colChkInfo.health;
            e["targetable"] = (a->flags & ACTOR_FLAG_ATTENTION_ENABLED) != 0;
            enemies.push_back(e);
            if (enemies.size() >= 16) {
                break;
            }
        }
    }
    nlohmann::json payload;
    payload["type"] = ENEMY_STATE;
    payload["targetClientId"] = clientId;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["enemies"] = enemies;
    payload["quiet"] = true;
    SendJsonToRemote(payload);
}
