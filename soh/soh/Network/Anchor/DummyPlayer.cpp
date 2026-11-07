#include "Anchor.h"
#include <algorithm>
#include <unordered_map>
#include "soh/Enhancements/nametag.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

static DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_FROZEN),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIFIED),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_FROZEN),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIFIED),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

static ColliderCylinderInit sBuddySwordInit = {
    { COLTYPE_NONE, AT_ON | AT_TYPE_PLAYER, AC_NONE, OC1_NONE, OC2_NONE, COLSHAPE_CYLINDER },
    { ELEMTYPE_UNK2, { DMG_SLASH_KOKIRI, 0x00, 0x01 }, { 0, 0x00, 0x00 }, TOUCH_ON | TOUCH_SFX_NORMAL, BUMP_NONE,
      OCELEM_NONE },
    { 35, 40, 0, { 0, 0, 0 } },
};

static std::unordered_map<uint32_t, ColliderCylinder> sBuddySwords;

// A buddy's swing: a sword hitbox in front of it (all round it for a spin) for a few frames.
static void BuddyDummy_Swing(AnchorClient& client, Player* player, PlayState* play) {
    if (client.attackFrames == 0) {
        return;
    }
    client.attackFrames--;
    Actor* actor = &player->actor;
    bool fresh = !sBuddySwords.contains(client.clientId);
    ColliderCylinder& sword = sBuddySwords[client.clientId];
    if (fresh) {
        Collider_InitCylinder(play, &sword);
    }
    if (fresh || sword.base.actor != actor) {
        Collider_SetCylinder(play, &sword, actor, &sBuddySwordInit);
    }
    sword.info.toucher.dmgFlags = client.attackDmgFlags;
    sword.info.toucher.damage = client.attackDamage;
    f32 reach = client.attackSpin ? 0.0f : 38.0f;
    sword.dim.radius = client.attackSpin ? 80 : 32;
    sword.dim.height = 45;
    sword.dim.pos.x = (s16)(actor->world.pos.x + Math_SinS(actor->shape.rot.y) * reach);
    sword.dim.pos.y = (s16)(actor->world.pos.y + 5.0f);
    sword.dim.pos.z = (s16)(actor->world.pos.z + Math_CosS(actor->shape.rot.y) * reach);
    CollisionCheck_SetAT(play, &play->colChkCtx, &sword.base);
}

// Walking the way the app steers, with the game's collision.
static void BuddyDummy_Drive(AnchorClient& client, Player* player, PlayState* play) {
    Actor* actor = &player->actor;
    bool adult = client.linkAge == LINK_AGE_ADULT;
    actor->world.rot.y = client.driveYaw;
    actor->shape.rot.y = client.driveYaw;
    actor->shape.rot.x = actor->shape.rot.z = 0;
    actor->speedXZ = std::clamp(client.driveSpeed, 0.0f, adult ? 11.0f : 9.0f);
    actor->gravity = -1.0f;
    actor->minVelocityY = -20.0f;
    if (client.driveJumpSeq != client.driveJumpDone) {
        client.driveJumpDone = client.driveJumpSeq;
        if (actor->bgCheckFlags & BGCHECKFLAG_GROUND) {
            actor->velocity.y = std::clamp(client.driveJump, 0.0f, 12.0f);
        }
    }
    if (actor->bgCheckFlags & BGCHECKFLAG_WATER) {
        // swimming: float at the surface like Link does
        if (actor->yDistToWater > 30.0f) {
            actor->velocity.y = 2.0f;
            actor->gravity = 0.0f;
        } else if (actor->yDistToWater > 20.0f) {
            actor->velocity.y = 0.0f;
            actor->gravity = 0.0f;
        }
    }
    Actor_MoveXZGravity(actor);
    Actor_UpdateBgCheckInfo(play, actor, adult ? 26.0f : 20.0f, adult ? 18.0f : 15.0f, adult ? 56.0f : 40.0f,
                            1 | 2 | 4);
    if (actor->bgCheckFlags & BGCHECKFLAG_GROUND) {
        actor->velocity.y = 0.0f;
    }
    if ((play->gameplayFrames % 2) == 0) {
        Anchor::Instance->SendPacket_BuddyPos(client.clientId, actor);
    }
}

void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Hack to account for usage of gSaveContext in Player_Init
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;

    // #region modeled after EnTorch2_Init and Player_Init
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[client.linkAge]);

    // Prevent dummy players from holding a weapon trail effect slot, as they don't use it anyway
    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;

    play->func_11D54(player, play);
    // #endregion

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    if (client.isBuddy && CVarGetInteger(CVAR_REMOTE_ANCHOR("BuddyCombat"), 1)) {
        // Harkinian Buddy: enemies and bosses can hit this buddy too, like they hit you
        player->cylinder.base.acFlags |= AC_TYPE_ENEMY;
    }
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    if (client.isBuddy) {
        // The attacker is never told it hit something: many enemies assume whatever they hit is
        // you and knock you back or hurt you (a buddy getting hit used to hurt you too).
        player->cylinder.info.bumperFlags |= BUMP_NO_AT_INFO;
    }
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    gSaveContext.linkAge = originalAge;

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom && CVarGetInteger(CVAR_REMOTE_ANCHOR("ShowNameTags"), 1)) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), {});
    }
}

void Math_Vec3s_Copy(Vec3s* dest, Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        client.driveHidden = true;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&player->upperLimbRot, &client.upperLimbRot);
    // Harkinian Buddy: walking the game does itself. The app steers; the game's own collision
    // decides where the buddy ends up - walls stop it, floors carry it, it falls off ledges.
    bool driving = client.isBuddy && client.driveOn && !client.driveHidden &&
                   (u32)(play->gameplayFrames - client.driveFrame) < 20;
    if (driving) {
        BuddyDummy_Drive(client, player, play);
    } else {
        Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
        Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
        actor->velocity.y = 0.0f;
    }
    client.driveHidden = false;
    if (client.isBuddy) {
        BuddyDummy_Swing(client, player, play);
        if ((play->gameplayFrames % 10) == (client.clientId % 10)) {
            Anchor::Instance->SendPacket_EnemyState(client.clientId, actor->world.pos);
        }
    }
    player->skelAnime.jointTable = client.jointTable;
    player->skelAnime.movementFlags = client.movementFlags;
    Math_Vec3s_Copy(&player->skelAnime.prevTransl, &client.prevTransl);
    player->currentBoots = client.currentBoots;
    player->currentShield = client.currentShield;
    player->heldItemId = client.buttonItem0;
    player->currentTunic = client.currentTunic;
    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->itemAction = client.itemAction;
    player->heldItemAction = client.heldItemAction;
    player->invincibilityTimer = client.invincibilityTimer;
    player->unk_862 =
        (client.unk_862 > (s16)GID_MAXIMUM) ? (s16)GID_STONE_OF_AGONY : client.unk_862; // prevent OOB, show SoA if OOB
    player->unk_85C = client.unk_85C;
    player->av1.actionVar1 = client.actionVar1;

    // Apply animation movement (Copied from Player_ApplyAnimMovementScaledByAge)
    Vec3f diff;
    SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);

    if (driving) {
        diff.x = diff.y = diff.z = 0.0f; // the game moved it already
    }

    if (player->skelAnime.movementFlags & 1) {
        if (!LINK_IS_ADULT) {
            diff.x *= 0.64f;
            diff.z *= 0.64f;
        }

        player->actor.world.pos.x += diff.x * player->actor.scale.x;
        player->actor.world.pos.z += diff.z * player->actor.scale.z;
    }

    if (player->skelAnime.movementFlags & 2) {
        if (!(player->skelAnime.movementFlags & 4)) {
            diff.y *= player->ageProperties->unk_08;
        }

        player->actor.world.pos.y += diff.y * player->actor.scale.y;
    }

    if (player->modelGroup != client.modelGroup) {
        // Hack to account for usage of gSaveContext
        s32 originalAge = gSaveContext.linkAge;
        gSaveContext.linkAge = client.linkAge;
        u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
        gSaveContext.equips.buttonItems[0] = client.buttonItem0;
        Player_SetModelGroup(player, client.modelGroup);
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
    }

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default"))) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    // Who hit it: you (your sword, or an arrow / boomerang / hookshot / bomb you let fly) is the
    // usual PvP hit; anything else (an enemy, a boss, a trap) is an enemy hit on a buddy.
    Actor* attacker = player->cylinder.base.ac;
    Actor* you = &GET_PLAYER(play)->actor;
    bool fromYou = attacker == nullptr || attacker == you || attacker->parent == you ||
                   attacker->id == ACTOR_PLAYER || attacker->id == ACTOR_EN_ARROW || attacker->id == ACTOR_EN_BOOM ||
                   attacker->id == ACTOR_ARMS_HOOK || attacker->id == ACTOR_EN_BOM || attacker->id == ACTOR_EN_BOM_CHU ||
                   attacker->id == ACTOR_MAGIC_FIRE || attacker->id == ACTOR_MAGIC_WIND || attacker->id == ACTOR_MAGIC_DARK;
    bool hitByEnemy = !fromYou;
    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0 && client.isBuddy &&
        hitByEnemy) {
        // an enemy (or boss, trap, hazard) hit a buddy: the app takes the hearts off and plays the hurt
        u8 damage = player->cylinder.info.acHitInfo != nullptr ? player->cylinder.info.acHitInfo->toucher.damage : 8;
        u8 effect = player->cylinder.info.acHitInfo != nullptr ? player->cylinder.info.acHitInfo->toucher.effect : 0;
        Anchor::Instance->SendPacket_BuddyHit(client.clientId, damage, effect, attacker->id, attacker->world.pos);
        Actor_SetColorFilter(&player->actor, 0x4000, 0xFF, 0, 8);
        player->invincibilityTimer = 20;
    } else if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0 && !hitByEnemy) {
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  player->actor.colChkInfo.damage);
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        return;
    }

    // Hack to account for usage of gSaveContext in Player_Draw
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = client.buttonItem0;

    Player_Draw((Actor*)player, play);
    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
    // DummyPlayer Actors are initially spawned as ACTOR_PLAYER, but change their
    // ID shortly afterwards to ACTOR_EN_OE2. This would cause ACTOR_PLAYER's
    // ActorDB Entry's `numLoaded` to leak, which is mostly harmless but hits debug
    // asserts. Set the id back to ACTOR_PLAYER so that `numLoaded` will be decremented
    // correctly.
    actor->id = ACTOR_PLAYER;

    // Drop client's pointer so packet handlers don't use after free
    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);
    if (Anchor::Instance->clients.contains(clientId) && Anchor::Instance->clients[clientId].player == (Player*)actor) {
        Anchor::Instance->clients[clientId].player = nullptr;
    }
}
