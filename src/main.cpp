/*
 * GD Swap Mode - Geode Mod for Geometry Dash 2.2
 * P2P LAN (Radmin VPN compatible)
 *
 * Features:
 * - Real UDP connection with handshake + timeout
 * - Configurable swap timer (min/max time)
 * - Auto-level teleport (host enters level → joiner auto-opens)
 * - Joiner locked in level during session
 * - Input relay (active player's inputs sent to spectator)
 * - HUD: timer, warnings, swap animation
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

// ── Winsock ──
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

// ═══════════════════════════════════════════
// Network Manager
// ═══════════════════════════════════════════

class NetworkManager {
public:
  static NetworkManager *get() {
    static NetworkManager inst;
    return &inst;
  }

  enum class State { Disconnected, Hosting, Connecting, Connected };

  // ── Connection ──
  State m_state = State::Disconnected;
  bool m_isHost = false;
  float m_connectTimeout = 0.f;

  // ── Game state ──
  int m_activePlayer = 1; // 1=host active, 2=joiner active
  bool m_inLevel = false;

  // ── Settings ──
  float m_minSwapTime = 5.f;
  float m_maxSwapTime = 15.f;
  bool m_joinerCanStartLevels = false;

  // ── Swap timer ──
  float m_swapTimer = 0.f;
  float m_nextSwapTime = 0.f;
  int m_warningSeconds = 0;
  bool m_swapJustHappened = false;

  // ── Remote inputs ──
  bool m_remoteJump = false;
  bool m_remoteRelease = false;
  bool m_remoteP2Jump = false;
  bool m_remoteP2Release = false;

  // ── Level sync ──
  int m_pendingLevelID = 0;
  bool m_needOpenLevel = false;

  // ── Network ──
  SOCKET m_sock = INVALID_SOCKET;
  sockaddr_in m_peerAddr{};
  bool m_peerKnown = false;
  std::atomic<bool> m_running{false};
  std::thread m_recvThread;
  std::mutex m_mtx;
  std::mt19937 m_rng;

  // ── UI feedback ──
  std::string m_statusMsg = "Nie polaczono";
  int m_statusColor = 0; // 0=gray,1=yellow,2=green,3=red

  bool isConnected() { return m_state == State::Connected; }

  // ────── Init ──────

  void initWS() {
#ifdef _WIN32
    static bool d = false;
    if (!d) {
      WSADATA w;
      WSAStartup(MAKEWORD(2, 2), &w);
      d = true;
    }
#endif
  }

  // ────── Host ──────

  bool hostSession() {
    initWS();
    disconnect();

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
      setStatus("Blad socketa!", 3);
      return false;
    }

    int y = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&y, sizeof(y));

#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(SWAP_PORT);
    a.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_sock, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
      setStatus("Port 5055 zajety!", 3);
      log::error("bind() failed");
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

    setStatus("Hostujesz! Czekam na kolege...", 1);
    log::info("HOST on port {}", SWAP_PORT);
    return true;
  }

  // ────── Join ──────

  bool joinSession(const std::string &ip) {
    initWS();
    disconnect();

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
      setStatus("Blad socketa!", 3);
      return false;
    }

#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

    m_peerAddr.sin_family = AF_INET;
    m_peerAddr.sin_port = htons(SWAP_PORT);
    if (inet_pton(AF_INET, ip.c_str(), &m_peerAddr.sin_addr) != 1) {
      setStatus("Nieprawidlowy IP!", 3);
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

    send("HELLO");
    setStatus("Laczenie z " + ip + "...", 1);
    log::info("JOIN {}:{}", ip, SWAP_PORT);
    return true;
  }

  // ────── Disconnect ──────

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
    m_needOpenLevel = false;
    m_pendingLevelID = 0;
    setStatus("Nie polaczono", 0);
  }

  // ────── Send ──────

  void send(const std::string &msg) {
    if (m_sock == INVALID_SOCKET || !m_peerKnown)
      return;
    sendto(m_sock, msg.c_str(), (int)msg.size(), 0, (sockaddr *)&m_peerAddr,
           sizeof(m_peerAddr));
  }

  // ────── Connection timeout ──────

  void updateConnection(float dt) {
    if (m_state == State::Connecting) {
      m_connectTimeout += dt;
      // Retry HELLO every 2 seconds
      if ((int)m_connectTimeout % 2 == 0 &&
          m_connectTimeout - (int)m_connectTimeout < dt) {
        send("HELLO");
      }
      if (m_connectTimeout >= 10.f) {
        setStatus("Timeout! Host nie odpowiada.", 3);
        disconnect();
      }
    }
  }

  // ────── Game logic ──────

  bool isActivePlayer() {
    if (!isConnected())
      return true;
    return (m_isHost && m_activePlayer == 1) ||
           (!m_isHost && m_activePlayer == 2);
  }

  void updateSwapTimer(float dt) {
    if (!m_isHost || !isConnected() || !m_inLevel)
      return;

    m_swapTimer += dt;
    float tl = m_nextSwapTime - m_swapTimer;

    if (tl <= 3.f && tl > 0.f) {
      int s = (int)std::ceil(tl);
      if (s != m_warningSeconds && s >= 1 && s <= 3) {
        m_warningSeconds = s;
        send("W " + std::to_string(s));
      }
    }

    if (m_swapTimer >= m_nextSwapTime) {
      m_activePlayer = (m_activePlayer == 1) ? 2 : 1;
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      resetTimer();
      send("S " + std::to_string(m_activePlayer));
      log::info("=== SWAP === Active: P{}", m_activePlayer);
    }
  }

  void resetTimer() {
    m_swapTimer = 0.f;
    std::uniform_real_distribution<float> d(m_minSwapTime, m_maxSwapTime);
    m_nextSwapTime = d(m_rng);
    log::info("Next swap: {:.1f}s", m_nextSwapTime);
  }

  float getTimeLeft() { return m_nextSwapTime - m_swapTimer; }

  void onEnterLevel(GJGameLevel *level) {
    m_inLevel = true;
    m_activePlayer = 1;
    m_swapJustHappened = false;
    m_warningSeconds = 0;

    if (!isConnected())
      return;

    int lid = level->m_levelID.value();

    if (m_isHost) {
      resetTimer();
      // Tell joiner to open this level
      send("LO " + std::to_string(lid));
      log::info("HOST entered level {}, timer={:.1f}s", lid, m_nextSwapTime);
    } else {
      send("JL " + std::to_string(lid));
      log::info("JOINER entered level {}", lid);
    }
  }

  void onExitLevel() {
    m_inLevel = false;
    if (isConnected())
      send("LL");
  }

  void sendJump() { send("J"); }
  void sendRelease() { send("R"); }
  void sendP2Jump() { send("J2"); }
  void sendP2Release() { send("R2"); }

  // ────── Auto-open level ──────

  void tryOpenLevel(int levelID) {
    log::info("Auto-opening level {}", levelID);

    geode::Loader::get()->queueInMainThread([levelID]() {
      // Already in a level? Skip
      if (auto pl = PlayLayer::get()) {
        log::info("Already in a level, skipping auto-open");
        return;
      }

      auto glm = GameLevelManager::sharedState();
      GJGameLevel *level = nullptr;

      // Try main level (1-23)
      if (levelID >= 1 && levelID <= 23) {
        level = glm->getMainLevel(levelID, false);
      }

      // Try saved/downloaded level
      if (!level) {
        level = glm->getSavedLevel(levelID);
      }

      if (!level) {
        log::warn("Cannot find level {}", levelID);
        FLAlertLayer::create(
            "GD Swap",
            "Nie znaleziono levela! Wejdz recznie w ten sam level co host.",
            "OK")
            ->show();
        return;
      }

      auto scene = cocos2d::CCScene::create();
      scene->addChild(PlayLayer::create(level, false, false));
      cocos2d::CCDirector::sharedDirector()->replaceScene(
          cocos2d::CCTransitionFade::create(0.5f, scene));
    });
  }

private:
  NetworkManager() {}

  void setStatus(const std::string &s, int c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_statusMsg = s;
    m_statusColor = c;
  }

  void recvLoop() {
    char buf[256];
    sockaddr_in from{};
    int fl = sizeof(from);

    while (m_running) {
      int n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
      if (n <= 0)
        continue;
      buf[n] = '\0';
      std::string msg(buf);

      // Host: accept peer from HELLO
      if (m_isHost && m_state == State::Hosting && msg == "HELLO") {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_peerAddr = from;
        m_peerKnown = true;
        m_state = State::Connected;
        send("WELCOME");
        setStatus("Kolega polaczony!", 2);
        log::info("HOST: peer connected");
        continue;
      }

      // Joiner: confirm connection on WELCOME
      if (!m_isHost && m_state == State::Connecting && msg == "WELCOME") {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_state = State::Connected;
        setStatus("Polaczono z hostem!", 2);
        log::info("JOINER: connected!");
        continue;
      }

      if (m_state == State::Connected) {
        handleMsg(msg);
      }
    }
  }

  void handleMsg(const std::string &msg) {
    std::lock_guard<std::mutex> lk(m_mtx);

    if (msg.rfind("LO ", 0) == 0) {
      // Level Open — host entered a level, joiner should auto-open
      int lid = std::stoi(msg.substr(3));
      log::info("Received LEVEL_OPEN {}", lid);
      if (!m_inLevel) {
        tryOpenLevel(lid);
      }
    } else if (msg.rfind("JL ", 0) == 0) {
      log::info("Joiner entered level");
    } else if (msg == "LL") {
      log::info("Peer left level");
    } else if (msg.rfind("S ", 0) == 0) {
      m_activePlayer = std::stoi(msg.substr(2));
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      log::info("SWAP! active=P{}", m_activePlayer);
    } else if (msg.rfind("W ", 0) == 0) {
      m_warningSeconds = std::stoi(msg.substr(2));
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
};

// ═══════════════════════════════════════════
// Swap Connect Popup
// ═══════════════════════════════════════════

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
    auto s = m_mainLayer->getContentSize();

    // ── IP ──
    auto ipL = CCLabelBMFont::create("IP kolegi:", "bigFont.fnt");
    ipL->setScale(0.32f);
    ipL->setPosition(ccp(s.width / 2, s.height - 50));
    m_mainLayer->addChild(ipL);

    m_ipInput = geode::TextInput::create(200.f, "np. 127.0.0.1");
    m_ipInput->setPosition(ccp(s.width / 2, s.height - 72));
    m_ipInput->setFilter("0123456789.");
    m_ipInput->setMaxCharCount(15);
    m_mainLayer->addChild(m_ipInput);

    // ── Swap time ──
    auto stL = CCLabelBMFont::create("Czas swapu (sekundy):", "bigFont.fnt");
    stL->setScale(0.27f);
    stL->setPosition(ccp(s.width / 2, s.height - 98));
    m_mainLayer->addChild(stL);

    auto mnL = CCLabelBMFont::create("Min:", "bigFont.fnt");
    mnL->setScale(0.27f);
    mnL->setPosition(ccp(s.width / 2 - 60, s.height - 116));
    m_mainLayer->addChild(mnL);

    m_minInput = geode::TextInput::create(50.f, "5");
    m_minInput->setPosition(ccp(s.width / 2 - 20, s.height - 116));
    m_minInput->setFilter("0123456789");
    m_minInput->setMaxCharCount(3);
    m_minInput->setString("5");
    m_mainLayer->addChild(m_minInput);

    auto mxL = CCLabelBMFont::create("Max:", "bigFont.fnt");
    mxL->setScale(0.27f);
    mxL->setPosition(ccp(s.width / 2 + 40, s.height - 116));
    m_mainLayer->addChild(mxL);

    m_maxInput = geode::TextInput::create(50.f, "15");
    m_maxInput->setPosition(ccp(s.width / 2 + 80, s.height - 116));
    m_maxInput->setFilter("0123456789");
    m_maxInput->setMaxCharCount(3);
    m_maxInput->setString("15");
    m_mainLayer->addChild(m_maxInput);

    // ── Status ──
    m_statusLabel = CCLabelBMFont::create("Nie polaczono", "bigFont.fnt");
    m_statusLabel->setScale(0.27f);
    m_statusLabel->setColor(ccc3(200, 200, 200));
    m_statusLabel->setPosition(ccp(s.width / 2, s.height - 140));
    m_mainLayer->addChild(m_statusLabel);

    // Info note
    auto note = CCLabelBMFont::create(
        "Host wchodzi na level -> joiner wejdzie automatycznie!",
        "bigFont.fnt");
    note->setScale(0.18f);
    note->setColor(ccc3(255, 200, 100));
    note->setPosition(ccp(s.width / 2, s.height - 156));
    m_mainLayer->addChild(note);

    // ── Buttons ──
    auto hS = ButtonSprite::create("Hostuj", "goldFont.fnt", "GJ_button_01.png",
                                   0.8f);
    auto hB = CCMenuItemSpriteExtra::create(
        hS, this, menu_selector(SwapConnectPopup::onHost));
    hB->setPosition(ccp(s.width / 2 - 70, 48));
    m_buttonMenu->addChild(hB);

    auto jS = ButtonSprite::create("Dolacz", "goldFont.fnt", "GJ_button_02.png",
                                   0.8f);
    auto jB = CCMenuItemSpriteExtra::create(
        jS, this, menu_selector(SwapConnectPopup::onJoin));
    jB->setPosition(ccp(s.width / 2 + 70, 48));
    m_buttonMenu->addChild(jB);

    auto dS = ButtonSprite::create("Rozlacz", "goldFont.fnt",
                                   "GJ_button_06.png", 0.7f);
    auto dB = CCMenuItemSpriteExtra::create(
        dS, this, menu_selector(SwapConnectPopup::onDisconnect));
    dB->setPosition(ccp(s.width / 2, 20));
    m_buttonMenu->addChild(dB);

    this->schedule(schedule_selector(SwapConnectPopup::tick), 0.3f);
    return true;
  }

  void applySettings() {
    auto net = NetworkManager::get();
    std::string mn(m_minInput->getString());
    std::string mx(m_maxInput->getString());
    float a = mn.empty() ? 5.f : std::stof(mn);
    float b = mx.empty() ? 15.f : std::stof(mx);
    if (a < 1.f)
      a = 1.f;
    if (b <= a)
      b = a + 1.f;
    net->m_minSwapTime = a;
    net->m_maxSwapTime = b;
  }

  void onHost(CCObject *) {
    auto net = NetworkManager::get();
    if (net->m_state != NetworkManager::State::Disconnected)
      return;
    applySettings();
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
    net->updateConnection(dt);
    std::lock_guard<std::mutex> lk(net->m_mtx);
    m_statusLabel->setString(net->m_statusMsg.c_str());
    switch (net->m_statusColor) {
    case 0:
      m_statusLabel->setColor(ccc3(200, 200, 200));
      break;
    case 1:
      m_statusLabel->setColor(ccc3(255, 255, 100));
      break;
    case 2:
      m_statusLabel->setColor(ccc3(100, 255, 100));
      break;
    case 3:
      m_statusLabel->setColor(ccc3(255, 80, 80));
      break;
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

// ═══════════════════════════════════════════
// Menu Hook — SWAP button
// ═══════════════════════════════════════════

class $modify(SwapMenuLayer, MenuLayer) {
  bool init() {
    if (!MenuLayer::init())
      return false;

    auto ws = CCDirector::sharedDirector()->getWinSize();
    auto lab = CCLabelBMFont::create("SWAP", "bigFont.fnt");
    lab->setScale(0.45f);
    auto spr = CircleButtonSprite::create(lab, CircleBaseColor::Green,
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

// ═══════════════════════════════════════════
// PlayLayer Hook — swap HUD + logic
// ═══════════════════════════════════════════

class $modify(SwapPlayLayer, PlayLayer) {
  struct Fields {
    CCLabelBMFont *m_swapLbl = nullptr;
    CCLabelBMFont *m_modeLbl = nullptr;
    CCLabelBMFont *m_dbgLbl = nullptr;
    float m_displayTmr = 0.f;
    int m_lastWarn = 0;
  };

  bool init(GJGameLevel *level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects))
      return false;

    auto net = NetworkManager::get();
    if (!net->isConnected())
      return true;

    auto ws = CCDirector::sharedDirector()->getWinSize();

    // Swap notification (center top)
    m_fields->m_swapLbl = CCLabelBMFont::create("", "bigFont.fnt");
    m_fields->m_swapLbl->setPosition(ccp(ws.width / 2, ws.height - 30));
    m_fields->m_swapLbl->setScale(0.5f);
    m_fields->m_swapLbl->setOpacity(0);
    m_fields->m_swapLbl->setZOrder(9999);
    this->addChild(m_fields->m_swapLbl);

    // Mode (top-left)
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
    updMode();

    log::info("PlayLayer init, host={}, timer={:.1f}", net->m_isHost,
              net->m_nextSwapTime);
    return true;
  }

  void onQuit() {
    auto net = NetworkManager::get();

    // Joiner is LOCKED in level during swap session
    if (net->isConnected() && !net->m_isHost) {
      FLAlertLayer::create("GD Swap",
                           "Nie mozesz wyjsc! Host kontroluje sesje.", "OK")
          ->show();
      return;
    }

    net->onExitLevel();
    PlayLayer::onQuit();
  }

  void update(float dt) {
    PlayLayer::update(dt);

    auto net = NetworkManager::get();
    if (!net->isConnected())
      return;

    // Swap timer (host only)
    net->updateSwapTimer(dt);

    // Debug display
    if (m_fields->m_dbgLbl) {
      if (net->m_isHost) {
        auto t =
            fmt::format("{:.0f}s P{}", net->getTimeLeft(), net->m_activePlayer);
        m_fields->m_dbgLbl->setString(t.c_str());
      } else {
        auto t = fmt::format("P{} {}", net->m_activePlayer,
                             net->isActivePlayer() ? "GRASZ" : "PATRZ");
        m_fields->m_dbgLbl->setString(t.c_str());
      }
    }

    // Swap event
    if (net->m_swapJustHappened) {
      net->m_swapJustHappened = false;
      m_fields->m_displayTmr = 2.5f;
      m_fields->m_lastWarn = 0;

      if (m_fields->m_swapLbl) {
        auto who = net->isActivePlayer() ? "TY GRASZ!" : "OGLADASZ!";
        m_fields->m_swapLbl->setString(("SWAP! " + std::string(who)).c_str());
        m_fields->m_swapLbl->setOpacity(255);
        m_fields->m_swapLbl->setColor(ccc3(255, 50, 50));
        m_fields->m_swapLbl->setScale(0.8f);
        m_fields->m_swapLbl->stopAllActions();
        m_fields->m_swapLbl->runAction(CCScaleTo::create(0.3f, 0.5f));
      }
      updMode();
    }

    // Warning
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

    // Fade out
    if (m_fields->m_displayTmr > 0.f) {
      m_fields->m_displayTmr -= dt;
      if (m_fields->m_displayTmr <= 0.f && net->m_warningSeconds == 0 &&
          m_fields->m_swapLbl) {
        m_fields->m_swapLbl->stopAllActions();
        m_fields->m_swapLbl->runAction(CCFadeOut::create(0.5f));
      }
    }

    // Remote inputs when spectating
    if (!net->isActivePlayer()) {
      if (net->m_remoteJump) {
        this->m_player1->pushButton(PlayerButton::Jump);
        net->m_remoteJump = false;
      }
      if (net->m_remoteRelease) {
        this->m_player1->releaseButton(PlayerButton::Jump);
        net->m_remoteRelease = false;
      }
      if (net->m_remoteP2Jump && this->m_player2) {
        this->m_player2->pushButton(PlayerButton::Jump);
        net->m_remoteP2Jump = false;
      }
      if (net->m_remoteP2Release && this->m_player2) {
        this->m_player2->releaseButton(PlayerButton::Jump);
        net->m_remoteP2Release = false;
      }
    }
  }

  void updMode() {
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

// ═══════════════════════════════════════════
// Input Hook — block/relay
// ═══════════════════════════════════════════

class $modify(SwapBaseGameLayer, GJBaseGameLayer) {
  void handleButton(bool push, int button, bool player1) {
    auto net = NetworkManager::get();
    if (!net->isConnected() || !net->m_inLevel) {
      GJBaseGameLayer::handleButton(push, button, player1);
      return;
    }

    if (net->isActivePlayer()) {
      GJBaseGameLayer::handleButton(push, button, player1);
      if (player1) {
        push ? net->sendJump() : net->sendRelease();
      } else {
        push ? net->sendP2Jump() : net->sendP2Release();
      }
    }
    // Inactive: block all local input
  }
};
