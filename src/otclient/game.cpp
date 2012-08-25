/*
 * Copyright (c) 2010-2012 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "game.h"
#include "localplayer.h"
#include "map.h"
#include "tile.h"
#include "creature.h"
#include "container.h"
#include "statictext.h"
#include <framework/core/eventdispatcher.h>
#include <framework/ui/uimanager.h>
#include <framework/core/application.h>
#include "luavaluecasts.h"
#include "protocolgame.h"
#include "protocolcodes.h"

Game g_game;

Game::Game()
{
    resetGameStates();
    m_clientVersion = 0;
}

void Game::terminate()
{
    resetGameStates();
    m_protocolGame = nullptr;
}

void Game::resetGameStates()
{
    m_online = false;
    m_denyBotCall = false;
    m_dead = false;
    m_serverBeat = 50;
    m_seq = 0;
    m_ping = -1;
    m_canReportBugs = false;
    m_fightMode = Otc::FightBalanced;
    m_chaseMode = Otc::DontChase;
    m_safeFight = true;
    m_followingCreature = nullptr;
    m_attackingCreature = nullptr;
    m_localPlayer = nullptr;

    for(auto& it : m_containers) {
        const ContainerPtr& container = it.second;
        if(container)
            container->onClose();
    }

    if(m_pingEvent) {
        m_pingEvent->cancel();
        m_pingEvent = nullptr;
    }

    m_containers.clear();
    m_vips.clear();
    m_gmActions.clear();
}

void Game::processConnectionError(const boost::system::error_code& ec)
{
    // connection errors only have meaning if we still have a protocol
    if(m_protocolGame) {
        // eof = end of file, a clean disconnect
        if(ec != asio::error::eof)
            g_lua.callGlobalField("g_game", "onConnectionError", ec.message(), ec.value());

        processDisconnect();
    }
}

void Game::processDisconnect()
{
    if(isOnline())
        processGameEnd();

    if(m_protocolGame) {
        m_protocolGame->disconnect();
        m_protocolGame = nullptr;
    }
}

void Game::processLoginError(const std::string& error)
{
    g_lua.callGlobalField("g_game", "onLoginError", error);
}

void Game::processLoginAdvice(const std::string& message)
{
    g_lua.callGlobalField("g_game", "onLoginAdvice", message);
}

void Game::processLoginWait(const std::string& message, int time)
{
    g_lua.callGlobalField("g_game", "onLoginWait", message, time);
}

void Game::processGameStart()
{
    m_online = true;

    // synchronize fight modes with the server
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight);

    // NOTE: the entire map description and local player information is not known yet (bot call is allowed here)
    enableBotCall();
    g_lua.callGlobalField("g_game", "onGameStart");
    disableBotCall();

    if(g_game.getFeature(Otc::GameClientPing)) {
        m_pingEvent = g_dispatcher.cycleEvent([this] {
            if(m_protocolGame && m_protocolGame->isConnected()) {
                enableBotCall();
                m_protocolGame->sendPing();
                disableBotCall();
            }
        }, 1000);
    }
}

void Game::processGameEnd()
{
    g_lua.callGlobalField("g_game", "onGameEnd");

    // reset game state
    resetGameStates();

    m_worldName = "";
    m_characterName = "";

    // clean map creatures
    g_map.cleanDynamicThings();
}

void Game::processDeath(int penality)
{
    m_dead = true;
    m_localPlayer->stopWalk();

    g_lua.callGlobalField("g_game", "onDeath", penality);
}

void Game::processGMActions(const std::vector<uint8>& actions)
{
    m_gmActions = actions;
    g_lua.callGlobalField("g_game", "onGMActions", actions);
}

void Game::processPing()
{
    g_lua.callGlobalField("g_game", "onPing");
}

void Game::processPingBack(int elapsed)
{
    m_ping = elapsed;
    g_lua.callGlobalField("g_game", "onPingBack", elapsed);
}

void Game::processTextMessage(Otc::MessageMode mode, const std::string& text)
{
    g_lua.callGlobalField("g_game", "onTextMessage", mode, text);
}

void Game::processTalk(const std::string& name, int level, Otc::MessageMode mode, const std::string& text, int channelId, const Position& pos)
{
    g_lua.callGlobalField("g_game", "onTalk", name, level, mode, text, channelId, pos);
}

void Game::processOpenContainer(int containerId, const ItemPtr& containerItem, const std::string& name, int capacity, bool hasParent, const std::vector<ItemPtr>& items)
{
    ContainerPtr previousContainer = getContainer(containerId);
    ContainerPtr container = ContainerPtr(new Container(containerId, capacity, name, containerItem, hasParent));
    m_containers[containerId] = container;
    container->onAddItems(items);

    // we might want to close a container here
    enableBotCall();
    container->onOpen(previousContainer);
    disableBotCall();

    if(previousContainer)
        previousContainer->onClose();
}

void Game::processCloseContainer(int containerId)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        g_logger.traceError("container not found");
        return;
    }

    m_containers[containerId] = nullptr;
    container->onClose();
}

void Game::processContainerAddItem(int containerId, const ItemPtr& item)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        g_logger.traceError("container not found");
        return;
    }

    container->onAddItem(item);
}

void Game::processContainerUpdateItem(int containerId, int slot, const ItemPtr& item)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        g_logger.traceError("container not found");
        return;
    }

    container->onUpdateItem(slot, item);
}

void Game::processContainerRemoveItem(int containerId, int slot)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        g_logger.traceError("container not found");
        return;
    }

    container->onRemoveItem(slot);
}

void Game::processInventoryChange(int slot, const ItemPtr& item)
{
    if(item)
        item->setPosition(Position(65535, slot, 0));

    m_localPlayer->setInventoryItem((Otc::InventorySlot)slot, item);
}

void Game::processChannelList(const std::vector<std::tuple<int, std::string> >& channelList)
{
    g_lua.callGlobalField("g_game", "onChannelList", channelList);
}

void Game::processOpenChannel(int channelId, const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenChannel", channelId, name);
}

void Game::processOpenPrivateChannel(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenPrivateChannel", name);
}

void Game::processOpenOwnPrivateChannel(int channelId, const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenOwnPrivateChannel", channelId, name);
}

void Game::processCloseChannel(int channelId)
{
    g_lua.callGlobalField("g_game", "onCloseChannel", channelId);
}

void Game::processRuleViolationChannel(int channelId)
{
    g_lua.callGlobalField("g_game", "onRuleViolationChannel", channelId);
}

void Game::processRuleViolationRemove(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onRuleViolationRemove", name);
}

void Game::processRuleViolationCancel(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onRuleViolationCancel", name);
}

void Game::processRuleViolationLock()
{
    g_lua.callGlobalField("g_game", "onRuleViolationLock");
}

void Game::processVipAdd(uint id, const std::string& name, bool online)
{
    m_vips[id] = Vip(name, online);
    g_lua.callGlobalField("g_game", "onAddVip", id, name, online);
}

void Game::processVipStateChange(uint id, bool online)
{
    std::get<1>(m_vips[id]) = online;
    g_lua.callGlobalField("g_game", "onVipStateChange", id, online);
}

void Game::processTutorialHint(int id)
{
    g_lua.callGlobalField("g_game", "onTutorialHint", id);
}

void Game::processAutomapFlag(const Position& pos, int icon, const std::string& message)
{
    g_lua.callGlobalField("g_game", "onAutomapFlag", pos, icon, message);
}

void Game::processOpenOutfitWindow(const Outfit& currentOufit, const std::vector<std::tuple<int, std::string, int> >& outfitList,
                                   const std::vector<std::tuple<int, std::string> >& mountList)
{
    // create virtual creature outfit
    CreaturePtr virtualOutfitCreature = CreaturePtr(new Creature);
    virtualOutfitCreature->setDirection(Otc::South);

    Outfit outfit = currentOufit;
    outfit.setMount(0);
    virtualOutfitCreature->setOutfit(outfit);

    // creature virtual mount outfit
    CreaturePtr virtualMountCreature = nullptr;
    if(getFeature(Otc::GamePlayerMounts))
    {
        virtualMountCreature = CreaturePtr(new Creature);
        virtualMountCreature->setDirection(Otc::South);

        Outfit mountOutfit;
        mountOutfit.setId(0);

        int mount = currentOufit.getMount();
        if(mount > 0)
            mountOutfit.setId(mount);

        virtualMountCreature->setOutfit(mountOutfit);
    }

    g_lua.callGlobalField("g_game", "onOpenOutfitWindow", virtualOutfitCreature, outfitList, virtualMountCreature, mountList);
}

void Game::processOpenNpcTrade(const std::vector<std::tuple<ItemPtr, std::string, int, int, int> >& items)
{
    g_lua.callGlobalField("g_game", "onOpenNpcTrade", items);
}

void Game::processPlayerGoods(int money, const std::vector<std::tuple<ItemPtr, int> >& goods)
{
    g_lua.callGlobalField("g_game", "onPlayerGoods", money, goods);
}

void Game::processCloseNpcTrade()
{
    g_lua.callGlobalField("g_game", "onCloseNpcTrade");
}

void Game::processOwnTrade(const std::string& name, const std::vector<ItemPtr>& items)
{
    g_lua.callGlobalField("g_game", "onOwnTrade", name, items);
}

void Game::processCounterTrade(const std::string& name, const std::vector<ItemPtr>& items)
{
    g_lua.callGlobalField("g_game", "onCounterTrade", name, items);
}

void Game::processCloseTrade()
{
    g_lua.callGlobalField("g_game", "onCloseTrade");
}

void Game::processEditText(uint id, int itemId, int maxLength, const std::string& text, const std::string& writter, const std::string& date)
{
    g_lua.callGlobalField("g_game", "onEditText", id, itemId, maxLength, text, writter, date);
}

void Game::processEditList(uint id, int doorId, const std::string& text)
{
    g_lua.callGlobalField("g_game", "onEditList", id, doorId, text);
}

void Game::processQuestLog(const std::vector<std::tuple<int, std::string, bool> >& questList)
{
    g_lua.callGlobalField("g_game", "onQuestLog", questList);
}

void Game::processQuestLine(int questId, const std::vector<std::tuple<std::string, std::string> >& questMissions)
{
    g_lua.callGlobalField("g_game", "onQuestLine", questId, questMissions);
}

void Game::processAttackCancel(uint seq)
{
    if(isAttacking() && (seq == 0 || m_seq == seq))
        cancelAttack();
}

void Game::processWalkCancel(Otc::Direction direction)
{
    if(m_localPlayer->isAutoWalking())
        m_protocolGame->sendStop();

    m_localPlayer->cancelWalk(direction);
}

void Game::loginWorld(const std::string& account, const std::string& password, const std::string& worldName, const std::string& worldHost, int worldPort, const std::string& characterName)
{
    if(m_protocolGame || isOnline())
        stdext::throw_exception("Unable to login into a world while already online or logging.");

    if(m_clientVersion == 0)
        stdext::throw_exception("Must set a valid game protocol version before logging.");

    // reset the new game state
    resetGameStates();

    m_localPlayer = LocalPlayerPtr(new LocalPlayer);
    m_localPlayer->setName(characterName);

    m_protocolGame = ProtocolGamePtr(new ProtocolGame);
    m_protocolGame->login(account, password, worldHost, (uint16)worldPort, characterName);
    m_characterName = characterName;
    m_worldName = worldName;
}

void Game::cancelLogin()
{
    // send logout even if the game has not started yet, to make sure that the player doesn't stay logged there
    if(m_protocolGame)
        m_protocolGame->sendLogout();

    processDisconnect();
}

void Game::forceLogout()
{
    if(!isOnline())
        return;

    m_protocolGame->sendLogout();
    processDisconnect();
}

void Game::safeLogout()
{
    if(!isOnline())
        return;

    m_protocolGame->sendLogout();
}

void Game::walk(Otc::Direction direction)
{
    if(!canPerformGameAction())
        return;

    // must cancel follow before any new walk
    if(isFollowing())
        cancelFollow();

    // msut cancel auto walking and wait next try
    if(m_localPlayer->isAutoWalking()) {
        m_protocolGame->sendStop();
        return;
    }

    if(!m_localPlayer->canWalk(direction))
        return;

    Position toPos = m_localPlayer->getPosition().translatedToDirection(direction);
    TilePtr toTile = g_map.getTile(toPos);
    // only do prewalks to walkable tiles (like grounds and not walls)
    if(toTile && toTile->isWalkable())
        m_localPlayer->preWalk(direction);
    // check walk to another floor (e.g: when above 3 parcels)
    else {
        // check if can walk to a lower floor
        auto canChangeFloorDown = [&]() -> bool {
            Position pos = toPos;
            if(!pos.down())
                return false;
            TilePtr toTile = g_map.getTile(pos);
            if(toTile && toTile->hasElevation(3))
                return true;
            return false;
        };

        // check if can walk to a higher floor
        auto canChangeFloorUp = [&]() -> bool {
            TilePtr fromTile = m_localPlayer->getTile();
            if(!fromTile || !fromTile->hasElevation(3))
                return false;
            Position pos = toPos;
            if(!pos.up())
                return false;
            TilePtr toTile = g_map.getTile(pos);
            if(!toTile || !toTile->isWalkable())
                return false;
            return true;
        };

        if(canChangeFloorDown() || canChangeFloorUp() ||
            (!toTile || toTile->isEmpty())) {
            m_localPlayer->lockWalk();
        } else
            return;
    }

    g_lua.callGlobalField("g_game", "onWalk", direction);

    forceWalk(direction);
}

void Game::autoWalk(const std::vector<Otc::Direction>& dirs)
{
    if(!canPerformGameAction())
        return;

    // protocol limits walk path up to 255 directions
    if(dirs.size() > 127) {
        g_logger.error("Auto walk path too great, the maximum number of directions is 127");
        return;
    }

    if(dirs.size() == 0)
        return;

    // must cancel follow before any new walk
    if(isFollowing())
        cancelFollow();

    Otc::Direction direction = dirs.front();
    if(m_localPlayer->canWalk(direction)) {
        TilePtr toTile = g_map.getTile(m_localPlayer->getPosition().translatedToDirection(direction));
        if(toTile && toTile->isWalkable() && !m_localPlayer->isAutoWalking())
            m_localPlayer->preWalk(direction);
    }

    g_lua.callGlobalField("g_game", "onAutoWalk", dirs);

    m_protocolGame->sendAutoWalk(dirs);
}

void Game::forceWalk(Otc::Direction direction)
{
    if(!canPerformGameAction())
        return;

    switch(direction) {
    case Otc::North:
        m_protocolGame->sendWalkNorth();
        break;
    case Otc::East:
        m_protocolGame->sendWalkEast();
        break;
    case Otc::South:
        m_protocolGame->sendWalkSouth();
        break;
    case Otc::West:
        m_protocolGame->sendWalkWest();
        break;
    case Otc::NorthEast:
        m_protocolGame->sendWalkNorthEast();
        break;
    case Otc::SouthEast:
        m_protocolGame->sendWalkSouthEast();
        break;
    case Otc::SouthWest:
        m_protocolGame->sendWalkSouthWest();
        break;
    case Otc::NorthWest:
        m_protocolGame->sendWalkNorthWest();
        break;
    default:
        break;
    }

    g_lua.callGlobalField("g_game", "onForceWalk", direction);
}

void Game::turn(Otc::Direction direction)
{
    if(!canPerformGameAction())
        return;

    switch(direction) {
    case Otc::North:
        m_protocolGame->sendTurnNorth();
        break;
    case Otc::East:
        m_protocolGame->sendTurnEast();
        break;
    case Otc::South:
        m_protocolGame->sendTurnSouth();
        break;
    case Otc::West:
        m_protocolGame->sendTurnWest();
        break;
    default:
        break;
    }
}

void Game::stop()
{
    if(!canPerformGameAction())
        return;

    if(isFollowing())
        cancelFollow();

    m_protocolGame->sendStop();
}

void Game::look(const ThingPtr& thing)
{
    if(!canPerformGameAction() || !thing)
        return;

    m_protocolGame->sendLook(thing->getPosition(), thing->getId(), thing->getStackpos());
}

void Game::move(const ThingPtr& thing, const Position& toPos, int count)
{
    if(count <= 0)
        count = 1;

    if(!canPerformGameAction() || !thing || thing->getPosition() == toPos)
        return;

    uint id = thing->getId();
    if(thing->isCreature()) {
        CreaturePtr creature = thing->static_self_cast<Creature>();
        id = Proto::Creature;
    }

    m_protocolGame->sendMove(thing->getPosition(), id, thing->getStackpos(), toPos, count);
}

void Game::moveToParentContainer(const ThingPtr& thing, int count)
{
    if(!canPerformGameAction() || !thing || count <= 0)
        return;

    Position position = thing->getPosition();
    move(thing, Position(position.x, position.y, 254), count);
}

void Game::rotate(const ThingPtr& thing)
{
    if(!canPerformGameAction() || !thing)
        return;

    m_protocolGame->sendRotateItem(thing->getPosition(), thing->getId(), thing->getStackpos());
}

void Game::use(const ThingPtr& thing)
{
    if(!canPerformGameAction() || !thing)
        return;

    Position pos = thing->getPosition();
    if(!pos.isValid()) // virtual item
        pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    // some itens, e.g. parcel, are not set as containers but they are.
    // always try to use these items in free container slots.
    m_protocolGame->sendUseItem(pos, thing->getId(), thing->getStackpos(), findEmptyContainerId());
}

void Game::useInventoryItem(int itemId)
{
    if(!canPerformGameAction() || !g_things.isValidDatId(itemId, ThingCategoryItem))
        return;

    Position pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    m_protocolGame->sendUseItem(pos, itemId, 0, 0);
}

void Game::useWith(const ItemPtr& item, const ThingPtr& toThing)
{
    if(!canPerformGameAction() || !item || !toThing)
        return;

    Position pos = item->getPosition();
    if(!pos.isValid()) // virtual item
        pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    if(toThing->isCreature())
        m_protocolGame->sendUseOnCreature(pos, item->getId(), item->getStackpos(), toThing->getId());
    else
        m_protocolGame->sendUseItemWith(pos, item->getId(), item->getStackpos(), toThing->getPosition(), toThing->getId(), toThing->getStackpos());
}

void Game::useInventoryItemWith(int itemId, const ThingPtr& toThing)
{
    if(!canPerformGameAction() || !toThing)
        return;

    Position pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    if(toThing->isCreature())
        m_protocolGame->sendUseOnCreature(pos, itemId, 0, toThing->getId());
    else
        m_protocolGame->sendUseItemWith(pos, itemId, 0, toThing->getPosition(), toThing->getId(), toThing->getStackpos());
}

void Game::open(const ItemPtr& item, const ContainerPtr& previousContainer)
{
    if(!canPerformGameAction() || !item)
        return;

    int id = 0;
    if(!previousContainer)
        id = findEmptyContainerId();
    else
        id = previousContainer->getId();

    m_protocolGame->sendUseItem(item->getPosition(), item->getId(), item->getStackpos(), id);
}

void Game::openParent(const ContainerPtr& container)
{
    if(!canPerformGameAction() || !container)
        return;

    m_protocolGame->sendUpContainer(container->getId());
}

void Game::close(const ContainerPtr& container)
{
    if(!canPerformGameAction() || !container)
        return;

    m_protocolGame->sendCloseContainer(container->getId());
}

void Game::refreshContainer()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRefreshContainer();
}

void Game::attack(CreaturePtr creature)
{
    if(!canPerformGameAction() || creature == m_localPlayer)
        return;

    // cancel when attacking again
    if(creature && creature == m_attackingCreature)
        creature = nullptr;

    if(creature && isFollowing())
        cancelFollow();

    setAttackingCreature(creature);
    m_seq++;
    m_protocolGame->sendAttack(creature ? creature->getId() : 0, m_seq);
}

void Game::follow(CreaturePtr creature)
{
    if(!canPerformGameAction() || creature == m_localPlayer)
        return;

    // cancel when following again
    if(creature && creature == m_followingCreature)
        creature = nullptr;

    if(creature && isAttacking())
        cancelAttack();

    setFollowingCreature(creature);
    m_seq++;
    m_protocolGame->sendFollow(creature ? creature->getId() : 0, m_seq);
}

void Game::cancelAttackAndFollow()
{
    if(!canPerformGameAction())
        return;

    m_protocolGame->sendCancelAttackAndFollow();
}

void Game::talk(const std::string& message)
{
    if(!canPerformGameAction() || message.empty())
        return;
    talkChannel(Otc::MessageSay, 0, message);
}

void Game::talkChannel(Otc::MessageMode mode, int channelId, const std::string& message)
{
    if(!canPerformGameAction() || message.empty())
        return;
    m_protocolGame->sendTalk(mode, channelId, "", message);
}

void Game::talkPrivate(Otc::MessageMode mode, const std::string& receiver, const std::string& message)
{
    if(!canPerformGameAction() || receiver.empty() || message.empty())
        return;
    m_protocolGame->sendTalk(mode, 0, receiver, message);
}

void Game::openPrivateChannel(const std::string& receiver)
{
    if(!canPerformGameAction() || receiver.empty())
        return;
    m_protocolGame->sendOpenPrivateChannel(receiver);
}

void Game::requestChannels()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestChannels();
}

void Game::joinChannel(int channelId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendJoinChannel(channelId);
}

void Game::leaveChannel(int channelId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendLeaveChannel(channelId);
}

void Game::closeNpcChannel()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCloseNpcChannel();
}

void Game::openOwnChannel()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOpenOwnChannel();
}

void Game::inviteToOwnChannel(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendInviteToOwnChannel(name);
}

void Game::excludeFromOwnChannel(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendExcludeFromOwnChannel(name);
}

void Game::partyInvite(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendInviteToParty(creatureId);
}

void Game::partyJoin(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendJoinParty(creatureId);
}

void Game::partyRevokeInvitation(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRevokeInvitation(creatureId);
}

void Game::partyPassLeadership(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendPassLeadership(creatureId);
}

void Game::partyLeave()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendLeaveParty();
}

void Game::partyShareExperience(bool active)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendShareExperience(active);
}

void Game::requestOutfit()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestOutfit();
}

void Game::changeOutfit(const Outfit& outfit)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendChangeOutfit(outfit);
}

void Game::addVip(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendAddVip(name);
}

void Game::removeVip(int playerId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRemoveVip(playerId);
}

void Game::setChaseMode(Otc::ChaseModes chaseMode)
{
    if(!canPerformGameAction())
        return;
    if(m_chaseMode == chaseMode)
        return;
    m_chaseMode = chaseMode;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight);
    g_lua.callGlobalField("g_game", "onChaseModeChange", chaseMode);
}

void Game::setFightMode(Otc::FightModes fightMode)
{
    if(!canPerformGameAction())
        return;
    if(m_fightMode == fightMode)
        return;
    m_fightMode = fightMode;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight);
    g_lua.callGlobalField("g_game", "onFightModeChange", fightMode);
}

void Game::setSafeFight(bool on)
{
    if(!canPerformGameAction())
        return;
    if(m_safeFight == on)
        return;
    m_safeFight = on;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight);
    g_lua.callGlobalField("g_game", "onSafeFightChange", on);
}

void Game::inspectNpcTrade(const ItemPtr& item)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendInspectNpcTrade(item->getId(), item->getCount());
}

void Game::buyItem(const ItemPtr& item, int amount, bool ignoreCapacity, bool buyWithBackpack)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendBuyItem(item->getId(), item->getSubType(), amount, ignoreCapacity, buyWithBackpack);
}

void Game::sellItem(const ItemPtr& item, int amount, bool ignoreEquipped)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendSellItem(item->getId(), item->getSubType(), amount, ignoreEquipped);
}

void Game::closeNpcTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCloseNpcTrade();
}

void Game::requestTrade(const ItemPtr& item, const CreaturePtr& creature)
{
    if(!canPerformGameAction() || !item || !creature)
        return;
    m_protocolGame->sendRequestTrade(item->getPosition(), item->getId(), item->getStackpos(), creature->getId());
}

void Game::inspectTrade(bool counterOffer, int index)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendInspectTrade(counterOffer, index);
}

void Game::acceptTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendAcceptTrade();
}

void Game::rejectTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRejectTrade();
}

void Game::editText(uint id, const std::string& text)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendEditText(id, text);
}

void Game::editList(uint id, int doorId, const std::string& text)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendEditList(id, doorId, text);
}

void Game::reportBug(const std::string& comment)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendBugReport(comment);
}

void Game::reportRuleVilation(const std::string& target, int reason, int action, const std::string& comment, const std::string& statement, int statementId, bool ipBanishment)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRuleVilation(target, reason, action, comment, statement, statementId, ipBanishment);
}

void Game::debugReport(const std::string& a, const std::string& b, const std::string& c, const std::string& d)
{
    m_protocolGame->sendDebugReport(a, b, c, d);
}

void Game::requestQuestLog()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestQuestLog();
}

void Game::requestQuestLine(int questId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestQuestLine(questId);
}

void Game::equipItem(const ItemPtr& item)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendEquipItem(item->getId(), item->getCountOrSubType());
}

void Game::mount(bool mount)
{
    if(!canPerformGameAction())
        return;

    m_protocolGame->sendMountStatus(mount);
}

void Game::requestItemInfo(const ItemPtr& item, int index)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestItemInfo(item->getId(), item->getSubType(), index);
}

void Game::ping()
{
    if(!m_protocolGame || !m_protocolGame->isConnected())
        return;

    m_denyBotCall = false;
    m_protocolGame->sendPing();
    m_denyBotCall = true;
}

bool Game::checkBotProtection()
{
#ifdef BOT_PROTECTION
    // accepts calls comming from a stacktrace containing only C++ functions,
    // if the stacktrace contains a lua function, then only accept if the engine is processing an input event
    if(m_denyBotCall && g_lua.isInCppCallback() && !g_app.isOnInputEvent()) {
        g_logger.error(g_lua.traceback("caught a lua call to a bot protected game function, the call was cancelled"));
        return false;
    }
#endif
    return true;
}

bool Game::canPerformGameAction()
{
    // we can only perform game actions if we meet these conditions:
    // - the game is online
    // - the local player exists
    // - the local player is not dead
    // - we have a game protocol
    // - the game protocol is connected
    // - its not a bot action
    return m_online && m_localPlayer && !m_dead && m_protocolGame && m_protocolGame->isConnected() && checkBotProtection();
}

void Game::setClientVersion(int version)
{
    if(m_clientVersion == version)
        return;

    if(isOnline())
        stdext::throw_exception("Unable to change client version while online");

    if(version != 0 && (version < 810 || version > 961))
        stdext::throw_exception(stdext::format("Protocol version %d not supported", version));

    m_features.reset();
    enableFeature(Otc::GameFormatCreatureName);

    if(version <= 810) {
        enableFeature(Otc::GameChargeableItems);
    }

    if(version >= 854) {
        enableFeature(Otc::GameProtocolChecksum);
        enableFeature(Otc::GameAccountNames);
        enableFeature(Otc::GameChallangeOnLogin);
        enableFeature(Otc::GameDoubleFreeCapacity);
        enableFeature(Otc::GameCreatureEmblems);
    }

    if(version >= 862) {
        enableFeature(Otc::GamePenalityOnDeath);
    }

    if(version >= 870) {
        enableFeature(Otc::GameDoubleExperience);
        enableFeature(Otc::GamePlayerMounts);
        enableFeature(Otc::GameSpellList);
    }

    if(version >= 910) {
        enableFeature(Otc::GameNameOnNpcTrade);
        enableFeature(Otc::GameTotalCapacity);
        enableFeature(Otc::GameSkillsBase);
        enableFeature(Otc::GamePlayerRegenerationTime);
        enableFeature(Otc::GameChannelPlayerList);
        enableFeature(Otc::GameEnvironmentEffect);
        enableFeature(Otc::GameItemAnimationPhase);
    }

    if(version >= 940) {
        enableFeature(Otc::GamePlayerMarket);
    }

    if(version >= 953) {
        enableFeature(Otc::GamePurseSlot);
        enableFeature(Otc::GameClientPing);
    }

    if(version >= 960) {
        enableFeature(Otc::GameSpritesU32);
        enableFeature(Otc::GameOfflineTrainingTime);
    }

    m_clientVersion = version;

    Proto::buildMessageModesMap(version);

    g_lua.callGlobalField("g_game", "onClientVersionChange", version);
}

void Game::setAttackingCreature(const CreaturePtr& creature)
{
    CreaturePtr oldCreature = m_attackingCreature;
    m_attackingCreature = creature;

    g_lua.callGlobalField("g_game", "onAttackingCreatureChange", creature, oldCreature);
}

void Game::setFollowingCreature(const CreaturePtr& creature)
{
    CreaturePtr oldCreature = m_followingCreature;
    m_followingCreature = creature;

    g_lua.callGlobalField("g_game", "onFollowingCreatureChange", creature, oldCreature);
}

std::string Game::formatCreatureName(const std::string& name)
{
    std::string formatedName = name;
    if(getFeature(Otc::GameFormatCreatureName) && name.length() > 0)
        formatedName[0] = stdext::upchar(formatedName[0]);
    return formatedName;
}

int Game::findEmptyContainerId()
{
    int id = 0;
    while(m_containers[id] != nullptr)
        id++;
    return id;
}
