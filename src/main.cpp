/*
 * GD Swap Mode — Geode Mod for Geometry Dash 2.2
 *
 * Architecture:
 *   recv thread → queueInMainThread → handleMsg on main thread
 *   ALL state changes happen on the main thread. Zero race conditions.
 */

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

using namespace geode::prelude;

#ifdef _WIN32
#include <WS2tcpip.h>
#include <WinSock2.h>

#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include <atomic>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

static constexpr int SWAP_PORT = 5055;

// ═════════════════════════════════════════
// NetworkManager
// ═════════════════════════════════════════

class NetworkManager {
public:
  static NetworkManager *get() {
    static NetworkManager inst;
    return &inst;
  }

  enum class State { Disconnected, Hosting, Connecting, Connected };
  State m_state = State::Disconnected;
  bool m_isHost = false;

  int m_activePlayer = 1; // 1 = host active, 2 = joiner active
  bool m_inLevel = false;

  // Settings
  float m_minSwapTime = 5.f;
  float m_maxSwapTime = 15.f;

  // Swap timer (host only, main thread)
  float m_swapTimer = 0.f;
  float m_nextSwapTime = 0.f;
  int m_warningSeconds = 0;
  bool m_swapJustHappened = false;

  // Remote inputs (main thread only)
  bool m_remoteJump = false;
  bool m_remoteRelease = false;
  bool m_remoteP2Jump = false;
  bool m_remoteP2Release = false;

  // Position sync (main thread only)
  float m_remoteX = 0.f;
  float m_remoteY = 0.f;
  float m_remoteRot = 0.f;
  bool m_hasRemotePos = false;
  int m_syncFrame = 0;

  // Level exit
  bool m_forceExit = false;

  // Connection timeout
  float m_connectTimeout = 0.f;

  // UI status
  std::string m_statusMsg = "Nie polaczono";
  int m_statusColor = 0; // 0=gray 1=yellow 2=green 3=red

  // Network
  SOCKET m_sock = INVALID_SOCKET;
  sockaddr_in m_peerAddr{};
  bool m_peerKnown = false;
  std::atomic<bool> m_running{false};
  std::thread m_recvThread;
  std::mt19937 m_rng;

  bool isConnected() { return m_state == State::Connected; }

  bool isActivePlayer() {
    if (!isConnected())
      return true;
    return (m_isHost && m_activePlayer == 1) ||
           (!m_isHost && m_activePlayer == 2);
  }

  // ─── Winsock init ───

  void initWS() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
      WSADATA w;
      WSAStartup(MAKEWORD(2, 2), &w);
      done = true;
    }
#endif
  }

  // ─── Host ───

  bool hostSession() {
    initWS();
    disconnect();

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
      m_statusMsg = "Blad socketa!";
      m_statusColor = 3;
      return false;
    }

    int yes = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
               sizeof(yes));
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SWAP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
      m_statusMsg = "Port 5055 zajety!";
      m_statusColor = 3;
      closesocket(m_sock);
      m_sock = INVALID_SOCKET;
      return false;
    }

    m_isHost = true;
    m_state = State::Hosting;
    m_peerKnown = false;
    m_activePlayer = 1;
    m_running = true;
    m_rng.seed(std::random_device{}());

    m_recvThread = std::thread([this]() { recvLoop(); });
    m_recvThread.detach();

    m_statusMsg = "Hostujesz! Czekam na kolege...";
    m_statusColor = 1;
    log::info("HOST listening on port {}", SWAP_PORT);
    return true;
  }

  // ─── Join ───

  bool joinSession(const std::string &ip) {
    initWS();
    disconnect();

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
      m_statusMsg = "Blad socketa!";
      m_statusColor = 3;
      return false;
    }

#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

    m_peerAddr.sin_family = AF_INET;
    m_peerAddr.sin_port = htons(SWAP_PORT);
    if (inet_pton(AF_INET, ip.c_str(), &m_peerAddr.sin_addr) != 1) {
      m_statusMsg = "Nieprawidlowy IP!";
      m_statusColor = 3;
      return false;
    }

    m_peerKnown = true;
    m_isHost = false;
    m_state = State::Connecting;
    m_connectTimeout = 0.f;
    m_activePlayer = 1;
    m_running = true;

    m_recvThread = std::thread([this]() { recvLoop(); });
    m_recvThread.detach();

    sendUDP("HELLO");
    m_statusMsg = "Laczenie z " + ip + "...";
    m_statusColor = 1;
    log::info("JOIN connecting to {}:{}", ip, SWAP_PORT);
    return true;
  }

  // ─── Disconnect ───

  void disconnect() {
    m_running = false;
    if (m_sock != INVALID_SOCKET) {
      closesocket(m_sock);
      m_sock = INVALID_SOCKET;
    }
    m_state = State::Disconnected;
    m_peerKnown = false;
    m_inLevel = false;
    m_activePlayer = 1;
    m_warningSeconds = 0;
    m_swapJustHappened = false;
    m_forceExit = false;
    m_hasRemotePos = false;
    m_remoteJump = m_remoteRelease = m_remoteP2Jump = m_remoteP2Release = false;
    m_statusMsg = "Nie polaczono";
    m_statusColor = 0;
  }

  // ─── Send UDP ───

  void sendUDP(const std::string &msg) {
    if (m_sock == INVALID_SOCKET || !m_peerKnown)
      return;
    sendto(m_sock, msg.c_str(), (int)msg.size(), 0, (sockaddr *)&m_peerAddr,
           sizeof(m_peerAddr));
  }

  // ─── Handle message (ALWAYS called on main thread via queueInMainThread) ───

  void handleMsg(const std::string &msg) {
    if (msg == "_CONNECTED") {
      m_state = State::Connected;
      m_statusMsg = m_isHost ? "Kolega polaczony!" : "Polaczono z hostem!";
      m_statusColor = 2;
      log::info("=== CONNECTED ===");
      return;
    }

    // Only handle game messages when connected
    if (!isConnected())
      return;

    if (msg.rfind("LO ", 0) == 0) {
      int lid = std::stoi(msg.substr(3));
      log::info("LEVEL_OPEN {}", lid);
      if (!m_inLevel)
        openLevel(lid);
    } else if (msg == "LL") {
      log::info("Peer left level");
      if (!m_isHost && m_inLevel) {
        m_forceExit = true;
        // Also try to force quit if we're in a PlayLayer right now
        if (auto pl = PlayLayer::get()) {
          m_inLevel = false;
          pl->onQuit();
        }
      }
    } else if (msg.rfind("S ", 0) == 0) {
      int p = std::stoi(msg.substr(2));
      m_activePlayer = p;
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      m_hasRemotePos = false; // reset position on swap
      log::info("=== SWAP === active=P{}", p);
    } else if (msg.rfind("W ", 0) == 0) {
      m_warningSeconds = std::stoi(msg.substr(2));
    } else if (msg.rfind("P ", 0) == 0) {
      // Position: P x y rot
      std::istringstream iss(msg.substr(2));
      int x, y, r;
      if (iss >> x >> y >> r) {
        m_remoteX = (float)x;
        m_remoteY = (float)y;
        m_remoteRot = (float)r;
        m_hasRemotePos = true;
      }
    } else if (msg == "J") {
      m_remoteJump = true;
    } else if (msg == "R") {
      m_remoteRelease = true;
    } else if (msg == "J2") {
      m_remoteP2Jump = true;
    } else if (msg == "R2") {
      m_remoteP2Release = true;
    }
  }

  // ─── Connection timeout (called from popup tick) ───

  void updateConnectionTimeout(float dt) {
    if (m_state == State::Connecting) {
      m_connectTimeout += dt;
      if (m_connectTimeout >= 10.f) {
        m_statusMsg = "Timeout! Host nie odpowiada.";
        m_statusColor = 3;
        disconnect();
      }
    }
  }

  // ─── Swap timer (host only, called from PlayLayer update) ───

  void updateSwapTimer(float dt) {
    if (!m_isHost || !isConnected() || !m_inLevel)
      return;

    m_swapTimer += dt;
    float tl = m_nextSwapTime - m_swapTimer;

    // Warnings at 3, 2, 1
    if (tl <= 3.f && tl > 0.f) {
      int s = (int)std::ceil(tl);
      if (s != m_warningSeconds && s >= 1 && s <= 3) {
        m_warningSeconds = s;
        sendUDP("W " + std::to_string(s));
      }
    }

    // SWAP!
    if (m_swapTimer >= m_nextSwapTime) {
      m_activePlayer = (m_activePlayer == 1) ? 2 : 1;
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      m_hasRemotePos = false;
      resetTimer();
      sendUDP("S " + std::to_string(m_activePlayer));
      log::info("=== SWAP === now P{}", m_activePlayer);
    }
  }

  void resetTimer() {
    m_swapTimer = 0.f;
    std::uniform_real_distribution<float> dist(m_minSwapTime, m_maxSwapTime);
    m_nextSwapTime = dist(m_rng);
    log::info("Next swap in {:.1f}s", m_nextSwapTime);
  }

  float getTimeLeft() { return m_nextSwapTime - m_swapTimer; }

  // ─── Enter/exit level ───

  void onEnterLevel(GJGameLevel *level) {
    m_inLevel = true;
    m_activePlayer = 1;
    m_swapJustHappened = false;
    m_warningSeconds = 0;
    m_hasRemotePos = false;
    m_forceExit = false;
    m_syncFrame = 0;

    if (!isConnected())
      return;
    int lid = level->m_levelID.value();
    if (m_isHost) {
      resetTimer();
      sendUDP("LO " + std::to_string(lid));
      log::info("HOST entered level {}, swap in {:.1f}s", lid, m_nextSwapTime);
    } else {
      sendUDP("JL");
      log::info("JOINER entered level {}", lid);
    }
  }

  void onExitLevel() {
    bool was = m_inLevel;
    m_inLevel = false;
    m_hasRemotePos = false;
    if (isConnected() && was) {
      sendUDP("LL");
      log::info("Left level");
    }
  }

  // ─── Position + input sending ───

  void sendPos(float x, float y, float rot) {
    m_syncFrame++;
    if (m_syncFrame % 2 != 0)
      return;
    char buf[64];
    snprintf(buf, sizeof(buf), "P %d %d %d", (int)x, (int)y, (int)rot);
    sendUDP(buf);
  }

  void sendJump() { sendUDP("J"); }
  void sendRelease() { sendUDP("R"); }
  void sendP2Jump() { sendUDP("J2"); }
  void sendP2Release() { sendUDP("R2"); }

  // ─── Open level programmatically ───

  void openLevel(int levelID) {
    if (PlayLayer::get()) {
      log::info("Already in level, skip open");
      return;
    }
    auto glm = GameLevelManager::sharedState();
    GJGameLevel *lv = nullptr;
    if (levelID >= 1 && levelID <= 23)
      lv = glm->getMainLevel(levelID, false);
    if (!lv)
      lv = glm->getSavedLevel(levelID);
    if (!lv) {
      log::warn("Level {} not found", levelID);
      FLAlertLayer::create("GD Swap", "Nie znaleziono levela!", "OK")->show();
      return;
    }
    log::info("Auto-opening level {}", levelID);
    auto scene = cocos2d::CCScene::create();
    scene->addChild(PlayLayer::create(lv, false, false));
    cocos2d::CCDirector::sharedDirector()->replaceScene(
        cocos2d::CCTransitionFade::create(0.5f, scene));
  }

private:
  NetworkManager() {}

  // ─── Recv loop (background thread) ───
  // ONLY reads UDP packets and dispatches to main thread.
  // NO state changes here except m_peerAddr for handshake.

  void recvLoop() {
    char buf[256];
    sockaddr_in from{};
    log::info("Recv loop started");

    while (m_running) {
      int fl = sizeof(from); // RESET every iteration!
      int n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
      if (n <= 0)
        continue;
      buf[n] = '\0';
      std::string msg(buf, n);

      // ── Handshake: Host receives HELLO ──
      if (m_isHost && m_state == State::Hosting && msg == "HELLO") {
        m_peerAddr = from;
        m_peerKnown = true;
        sendUDP("WELCOME");
        log::info("HOST: HELLO received, WELCOME sent");
        // Notify main thread of connection
        geode::Loader::get()->queueInMainThread(
            [this]() { handleMsg("_CONNECTED"); });
        continue;
      }

      // ── Handshake: Joiner receives WELCOME ──
      if (!m_isHost && m_state == State::Connecting && msg == "WELCOME") {
        log::info("JOINER: WELCOME received");
        geode::Loader::get()->queueInMainThread(
            [this]() { handleMsg("_CONNECTED"); });
        continue;
      }

      // ── All other messages → main thread ──
      geode::Loader::get()->queueInMainThread(
          [this, msg]() { handleMsg(msg); });
    }
    log::info("Recv loop ended");
  }
};

// ═════════════════════════════════════════
// Popup
// ═════════════════════════════════════════

class SwapConnectPopup : public geode::Popup {
protected:
  geode::TextInput *m_ipInput = nullptr;
  geode::TextInput *m_minInput = nullptr;
  geode::TextInput *m_maxInput = nullptr;
  CCLabelBMFont *m_statusLabel = nullptr;

  bool init() {
    if (!Popup::init(340.f, 270.f))
      return false;
    this->setTitle("GD Swap Mode");
    auto sz = m_mainLayer->getContentSize();

    auto ipL = CCLabelBMFont::create("IP kolegi:", "bigFont.fnt");
    ipL->setScale(0.32f);
    ipL->setPosition(ccp(sz.width / 2, sz.height - 50));
    m_mainLayer->addChild(ipL);

    m_ipInput = geode::TextInput::create(200.f, "np. 127.0.0.1");
    m_ipInput->setPosition(ccp(sz.width / 2, sz.height - 72));
    m_ipInput->setFilter("0123456789.");
    m_ipInput->setMaxCharCount(15);
    m_mainLayer->addChild(m_ipInput);

    auto stL = CCLabelBMFont::create("Czas swapu (sekundy):", "bigFont.fnt");
    stL->setScale(0.27f);
    stL->setPosition(ccp(sz.width / 2, sz.height - 98));
    m_mainLayer->addChild(stL);

    auto mnL = CCLabelBMFont::create("Min:", "bigFont.fnt");
    mnL->setScale(0.27f);
    mnL->setPosition(ccp(sz.width / 2 - 60, sz.height - 116));
    m_mainLayer->addChild(mnL);

    m_minInput = geode::TextInput::create(50.f, "5");
    m_minInput->setPosition(ccp(sz.width / 2 - 20, sz.height - 116));
    m_minInput->setFilter("0123456789");
    m_minInput->setMaxCharCount(3);
    m_minInput->setString("5");
    m_mainLayer->addChild(m_minInput);

    auto mxL = CCLabelBMFont::create("Max:", "bigFont.fnt");
    mxL->setScale(0.27f);
    mxL->setPosition(ccp(sz.width / 2 + 40, sz.height - 116));
    m_mainLayer->addChild(mxL);

    m_maxInput = geode::TextInput::create(50.f, "15");
    m_maxInput->setPosition(ccp(sz.width / 2 + 80, sz.height - 116));
    m_maxInput->setFilter("0123456789");
    m_maxInput->setMaxCharCount(3);
    m_maxInput->setString("15");
    m_mainLayer->addChild(m_maxInput);

    m_statusLabel = CCLabelBMFont::create("Nie polaczono", "bigFont.fnt");
    m_statusLabel->setScale(0.27f);
    m_statusLabel->setColor(ccc3(200, 200, 200));
    m_statusLabel->setPosition(ccp(sz.width / 2, sz.height - 142));
    m_mainLayer->addChild(m_statusLabel);

    auto note = CCLabelBMFont::create("Host wchodzi na level = joiner auto!",
                                      "bigFont.fnt");
    note->setScale(0.18f);
    note->setColor(ccc3(255, 200, 100));
    note->setPosition(ccp(sz.width / 2, sz.height - 158));
    m_mainLayer->addChild(note);

    auto hostS = ButtonSprite::create("Hostuj", "goldFont.fnt",
                                      "GJ_button_01.png", 0.8f);
    auto hostB = CCMenuItemSpriteExtra::create(
        hostS, this, menu_selector(SwapConnectPopup::onHost));
    hostB->setPosition(ccp(sz.width / 2 - 70, 48));
    m_buttonMenu->addChild(hostB);

    auto joinS = ButtonSprite::create("Dolacz", "goldFont.fnt",
                                      "GJ_button_02.png", 0.8f);
    auto joinB = CCMenuItemSpriteExtra::create(
        joinS, this, menu_selector(SwapConnectPopup::onJoin));
    joinB->setPosition(ccp(sz.width / 2 + 70, 48));
    m_buttonMenu->addChild(joinB);

    auto dcS = ButtonSprite::create("Rozlacz", "goldFont.fnt",
                                    "GJ_button_06.png", 0.7f);
    auto dcB = CCMenuItemSpriteExtra::create(
        dcS, this, menu_selector(SwapConnectPopup::onDisconnect));
    dcB->setPosition(ccp(sz.width / 2, 20));
    m_buttonMenu->addChild(dcB);

    this->schedule(schedule_selector(SwapConnectPopup::tick), 0.3f);
    return true;
  }

  void onHost(CCObject *) {
    auto net = NetworkManager::get();
    if (net->m_state != NetworkManager::State::Disconnected)
      return;
    // Apply settings
    std::string mn(m_minInput->getString()), mx(m_maxInput->getString());
    float a = mn.empty() ? 5.f : std::stof(mn);
    float b = mx.empty() ? 15.f : std::stof(mx);
    if (a < 1.f)
      a = 1.f;
    if (b <= a)
      b = a + 1.f;
    net->m_minSwapTime = a;
    net->m_maxSwapTime = b;
    net->hostSession();
  }

  void onJoin(CCObject *) {
    auto net = NetworkManager::get();
    if (net->m_state != NetworkManager::State::Disconnected)
      return;
    std::string ip(m_ipInput->getString());
    if (ip.empty()) {
      m_statusLabel->setString("Wpisz IP!");
      m_statusLabel->setColor(ccc3(255, 80, 80));
      return;
    }
    net->joinSession(ip);
  }

  void onDisconnect(CCObject *) { NetworkManager::get()->disconnect(); }

  void tick(float dt) {
    auto net = NetworkManager::get();
    net->updateConnectionTimeout(dt);

    m_statusLabel->setString(net->m_statusMsg.c_str());
    static const cocos2d::ccColor3B colors[] = {
        {200, 200, 200}, {255, 255, 100}, {100, 255, 100}, {255, 80, 80}};
    if (net->m_statusColor >= 0 && net->m_statusColor <= 3) {
      m_statusLabel->setColor(colors[net->m_statusColor]);
    }
  }

public:
  static SwapConnectPopup *create() {
    auto r = new SwapConnectPopup();
    if (r->init()) {
      r->autorelease();
      return r;
    }
    delete r;
    return nullptr;
  }
};

// ═════════════════════════════════════════
// Menu Hook — SWAP button
// ═════════════════════════════════════════

class $modify(SwapMenuLayer, MenuLayer) {
  bool init() {
    if (!MenuLayer::init())
      return false;

    auto ws = CCDirector::sharedDirector()->getWinSize();
    auto label = CCLabelBMFont::create("SWAP", "bigFont.fnt");
    label->setScale(0.45f);

    auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green,
                                          CircleBaseSize::Medium);
    auto btn = CCMenuItemSpriteExtra::create(
        spr, this, menu_selector(SwapMenuLayer::onSwap));
    auto menu = CCMenu::create();
    menu->addChild(btn);
    menu->setPosition(ccp(ws.width / 2, 45.f));
    this->addChild(menu, 10);

    return true;
  }

  void onSwap(CCObject *) { SwapConnectPopup::create()->show(); }
};

// ═════════════════════════════════════════
// PlayLayer Hook — HUD, timer, position sync
// ═════════════════════════════════════════

class $modify(SwapPlayLayer, PlayLayer) {
  struct Fields {
    CCLabelBMFont *m_swapLbl = nullptr;
    CCLabelBMFont *m_modeLbl = nullptr;
    CCLabelBMFont *m_dbgLbl = nullptr;
    float m_displayTmr = 0.f;
    int m_lastWarn = 0;
    bool m_active = false;
  };

  bool init(GJGameLevel *level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects))
      return false;

    auto net = NetworkManager::get();
    if (!net->isConnected())
      return true;

    m_fields->m_active = true;

    auto ws = CCDirector::sharedDirector()->getWinSize();

    // Swap notification (center top)
    m_fields->m_swapLbl = CCLabelBMFont::create("", "bigFont.fnt");
    m_fields->m_swapLbl->setPosition(ccp(ws.width / 2, ws.height - 30));
    m_fields->m_swapLbl->setScale(0.5f);
    m_fields->m_swapLbl->setOpacity(0);
    m_fields->m_swapLbl->setZOrder(9999);
    this->addChild(m_fields->m_swapLbl);

    // Mode label (top-left)
    m_fields->m_modeLbl = CCLabelBMFont::create("", "bigFont.fnt");
    m_fields->m_modeLbl->setPosition(ccp(80, ws.height - 15));
    m_fields->m_modeLbl->setScale(0.35f);
    m_fields->m_modeLbl->setZOrder(9999);
    this->addChild(m_fields->m_modeLbl);

    // Debug timer (top-right)
    m_fields->m_dbgLbl = CCLabelBMFont::create("", "chatFont.fnt");
    m_fields->m_dbgLbl->setPosition(ccp(ws.width - 60, ws.height - 15));
    m_fields->m_dbgLbl->setScale(0.6f);
    m_fields->m_dbgLbl->setZOrder(9999);
    m_fields->m_dbgLbl->setColor(ccc3(180, 180, 180));
    this->addChild(m_fields->m_dbgLbl);

    net->onEnterLevel(level);
    refreshModeLabel();

    log::info("=== PlayLayer INIT === host={} timer={:.1f}s ===", net->m_isHost,
              net->m_nextSwapTime);
    return true;
  }

  void onQuit() {
    auto net = NetworkManager::get();
    // JOINER: block manual exit, allow forced exit
    if (net->isConnected() && !net->m_isHost && !net->m_forceExit) {
      FLAlertLayer::create("GD Swap",
                           "Host kontroluje sesje! Nie mozesz wyjsc.", "OK")
          ->show();
      return;
    }
    net->m_forceExit = false;
    net->onExitLevel();
    PlayLayer::onQuit();
  }

  void postUpdate(float p0) {
    PlayLayer::postUpdate(p0);

    auto net = NetworkManager::get();
    if (!net->isConnected() || !m_fields->m_active)
      return;

    // ── Swap timer (host only) ──
    net->updateSwapTimer(p0);

    // ── Debug display ──
    if (m_fields->m_dbgLbl) {
      std::string dbg;
      if (net->m_isHost) {
        dbg =
            fmt::format("{:.0f}s P{}", net->getTimeLeft(), net->m_activePlayer);
      } else {
        dbg = fmt::format("P{} {}", net->m_activePlayer,
                          net->isActivePlayer() ? "GRA" : "PAT");
      }
      m_fields->m_dbgLbl->setString(dbg.c_str());
    }

    // ── Swap event ──
    if (net->m_swapJustHappened) {
      net->m_swapJustHappened = false;
      m_fields->m_displayTmr = 2.5f;
      m_fields->m_lastWarn = 0;

      if (m_fields->m_swapLbl) {
        auto txt =
            net->isActivePlayer() ? "SWAP! TY GRASZ!" : "SWAP! OGLADASZ!";
        m_fields->m_swapLbl->setString(txt);
        m_fields->m_swapLbl->setOpacity(255);
        m_fields->m_swapLbl->setColor(ccc3(255, 50, 50));
        m_fields->m_swapLbl->setScale(0.8f);
        m_fields->m_swapLbl->stopAllActions();
        m_fields->m_swapLbl->runAction(CCScaleTo::create(0.3f, 0.5f));
      }
      refreshModeLabel();
    }

    // ── Warning countdown ──
    if (net->m_warningSeconds > 0 &&
        net->m_warningSeconds != m_fields->m_lastWarn) {
      m_fields->m_lastWarn = net->m_warningSeconds;
      m_fields->m_displayTmr = 4.f;
      if (m_fields->m_swapLbl) {
        auto t = fmt::format("Zmiana za {}...", net->m_warningSeconds);
        m_fields->m_swapLbl->setString(t.c_str());
        m_fields->m_swapLbl->setOpacity(255);
        m_fields->m_swapLbl->setColor(ccc3(255, 255, 50));
        m_fields->m_swapLbl->setScale(0.55f);
      }
    }

    // ── Fade out notification ──
    if (m_fields->m_displayTmr > 0.f) {
      m_fields->m_displayTmr -= p0;
      if (m_fields->m_displayTmr <= 0.f && net->m_warningSeconds == 0) {
        if (m_fields->m_swapLbl) {
          m_fields->m_swapLbl->stopAllActions();
          m_fields->m_swapLbl->runAction(CCFadeOut::create(0.5f));
        }
      }
    }

    // ══════════════════════════════════
    // ACTIVE PLAYER → send position
    // ══════════════════════════════════
    if (net->isActivePlayer()) {
      if (this->m_player1) {
        auto pos = this->m_player1->getPosition();
        net->sendPos(pos.x, pos.y, this->m_player1->getRotation());
      }
    }
    // ══════════════════════════════════
    // SPECTATING → apply remote inputs + position
    // ══════════════════════════════════
    else {
      // Apply remote inputs
      if (this->m_player1) {
        if (net->m_remoteJump) {
          this->m_player1->pushButton(PlayerButton::Jump);
          net->m_remoteJump = false;
        }
        if (net->m_remoteRelease) {
          this->m_player1->releaseButton(PlayerButton::Jump);
          net->m_remoteRelease = false;
        }
      }
      if (this->m_player2) {
        if (net->m_remoteP2Jump) {
          this->m_player2->pushButton(PlayerButton::Jump);
          net->m_remoteP2Jump = false;
        }
        if (net->m_remoteP2Release) {
          this->m_player2->releaseButton(PlayerButton::Jump);
          net->m_remoteP2Release = false;
        }
      }
      // Teleport player to remote position
      if (net->m_hasRemotePos && this->m_player1) {
        this->m_player1->setPositionX(net->m_remoteX);
        this->m_player1->setPositionY(net->m_remoteY);
        this->m_player1->setRotation(net->m_remoteRot);
      }
    }
  }

  void refreshModeLabel() {
    if (!m_fields->m_modeLbl)
      return;
    auto net = NetworkManager::get();
    if (net->isActivePlayer()) {
      m_fields->m_modeLbl->setString("TY GRASZ!");
      m_fields->m_modeLbl->setColor(ccc3(100, 255, 100));
    } else {
      m_fields->m_modeLbl->setString("OGLADASZ");
      m_fields->m_modeLbl->setColor(ccc3(255, 100, 100));
    }
  }
};

// ═════════════════════════════════════════
// Input Hook — block inactive player
// ═════════════════════════════════════════

class $modify(SwapBaseGameLayer, GJBaseGameLayer) {
  void handleButton(bool push, int button, bool player1) {
    auto net = NetworkManager::get();

    // Not in swap session → normal input
    if (!net->isConnected() || !net->m_inLevel) {
      GJBaseGameLayer::handleButton(push, button, player1);
      return;
    }

    // Active player → allow + relay to peer
    if (net->isActivePlayer()) {
      GJBaseGameLayer::handleButton(push, button, player1);
      if (player1) {
        push ? net->sendJump() : net->sendRelease();
      } else {
        push ? net->sendP2Jump() : net->sendP2Release();
      }
    }
    // Inactive → BLOCK all local input
  }
};
