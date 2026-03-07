/*
 * GD Swap Mode - Geode Mod for Geometry Dash 2.2
 * P2P LAN version (Radmin VPN compatible)
 *
 * One player HOSTS (listens on UDP), the other CONNECTS to host's IP.
 * While playing any level, control randomly swaps between them every 20-50s.
 * A 3-second countdown appears before each swap.
 * The inactive player's inputs are blocked; they watch the active player.
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// ─────────────────────────────────────────
// P2P Network Manager (Direct UDP)
// ─────────────────────────────────────────

// WIN32_LEAN_AND_MEAN is defined via CMakeLists.txt compile definitions
// so the PCH won't include old winsock.h
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

#include <mutex>
#include <random>

static constexpr int SWAP_PORT = 5055;

class NetworkManager {
public:
  static NetworkManager *get() {
    static NetworkManager instance;
    return &instance;
  }

  // State
  bool m_connected = false;
  bool m_isHost = false;
  int m_activePlayer = 1; // 1=host plays, 2=guest plays
  bool m_inLevel = false;

  // Swap timer (managed by host)
  float m_swapTimer = 0.f;
  float m_nextSwapTime = 0.f;
  int m_warningSeconds = 0;
  bool m_swapJustHappened = false;
  bool m_showingWarning = false;

  // Remote input queue
  bool m_remoteJump = false;
  bool m_remoteRelease = false;
  bool m_remoteP2Jump = false;
  bool m_remoteP2Release = false;

  // Network
  SOCKET m_socket = INVALID_SOCKET;
  struct sockaddr_in m_peerAddr{};
  bool m_peerKnown = false;
  bool m_running = false;
  std::thread m_recvThread;
  std::mutex m_mutex;

  // RNG
  std::mt19937 m_rng;

  void initWinsock() {
#ifdef _WIN32
    static bool wsaInit = false;
    if (!wsaInit) {
      WSADATA wsaData;
      WSAStartup(MAKEWORD(2, 2), &wsaData);
      wsaInit = true;
    }
#endif
  }

  // HOST: bind and listen on a port
  bool hostSession() {
    initWinsock();
    disconnect();

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
      return false;

    // Allow reuse
    int yes = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
               sizeof(yes));

    struct sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(SWAP_PORT);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) ==
        SOCKET_ERROR) {
      closesocket(m_socket);
      m_socket = INVALID_SOCKET;
      return false;
    }

    // Set non-blocking recv timeout
#ifdef _WIN32
    DWORD timeout = 200;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
               sizeof(timeout));
#endif

    m_isHost = true;
    m_connected = true;
    m_peerKnown = false;
    m_activePlayer = 1;
    m_running = true;
    m_rng.seed(std::random_device{}());

    m_recvThread = std::thread([this]() { receiveLoop(); });
    m_recvThread.detach();

    log::info("Hosting swap session on port {}", SWAP_PORT);
    return true;
  }

  // CLIENT: connect to host IP
  bool joinSession(const std::string &hostIP) {
    initWinsock();
    disconnect();

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
      return false;

#ifdef _WIN32
    DWORD timeout = 200;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
               sizeof(timeout));
#endif

    m_peerAddr.sin_family = AF_INET;
    m_peerAddr.sin_port = htons(SWAP_PORT);
    inet_pton(AF_INET, hostIP.c_str(), &m_peerAddr.sin_addr);
    m_peerKnown = true;

    m_isHost = false;
    m_connected = true;
    m_activePlayer = 1; // host starts
    m_running = true;

    m_recvThread = std::thread([this]() { receiveLoop(); });
    m_recvThread.detach();

    // Send HELLO so the host knows our address
    sendToPeer("HELLO");

    log::info("Joining swap session at {}:{}", hostIP, SWAP_PORT);
    return true;
  }

  void disconnect() {
    m_running = false;
    m_connected = false;
    m_peerKnown = false;
    m_inLevel = false;
    m_activePlayer = 1;
    m_warningSeconds = 0;
    m_swapJustHappened = false;

    if (m_socket != INVALID_SOCKET) {
      closesocket(m_socket);
      m_socket = INVALID_SOCKET;
    }
  }

  void sendToPeer(const std::string &msg) {
    if (m_socket == INVALID_SOCKET || !m_peerKnown)
      return;
    sendto(m_socket, msg.c_str(), (int)msg.size(), 0,
           (struct sockaddr *)&m_peerAddr, sizeof(m_peerAddr));
  }

  bool isActivePlayer() {
    return (m_isHost && m_activePlayer == 1) ||
           (!m_isHost && m_activePlayer == 2);
  }

  int myPlayerNumber() { return m_isHost ? 1 : 2; }

  // Called from PlayLayer::update on host side to manage the swap timer
  void updateSwapTimer(float dt) {
    if (!m_isHost || !m_connected || !m_peerKnown || !m_inLevel)
      return;

    m_swapTimer += dt;

    float timeLeft = m_nextSwapTime - m_swapTimer;

    // Send warning at 3, 2, 1
    if (timeLeft <= 3.0f && timeLeft > 0.0f) {
      int sec = (int)std::ceil(timeLeft);
      if (sec != m_warningSeconds && sec >= 1 && sec <= 3) {
        m_warningSeconds = sec;
        m_showingWarning = true;
        sendToPeer("WARNING " + std::to_string(sec));
      }
    }

    // Time to swap!
    if (m_swapTimer >= m_nextSwapTime) {
      m_activePlayer = (m_activePlayer == 1) ? 2 : 1;
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      m_showingWarning = false;
      resetSwapTimer();
      sendToPeer("SWAP " + std::to_string(m_activePlayer));
      log::info("SWAP! Active player: {}", m_activePlayer);
    }
  }

  void resetSwapTimer() {
    m_swapTimer = 0.f;
    std::uniform_real_distribution<float> dist(20.f, 50.f);
    m_nextSwapTime = dist(m_rng);
    log::info("Next swap in {:.1f}s", m_nextSwapTime);
  }

  void onEnterLevel() {
    m_inLevel = true;
    m_activePlayer = 1; // host starts
    m_swapJustHappened = false;
    m_warningSeconds = 0;
    m_showingWarning = false;

    if (m_isHost) {
      resetSwapTimer();
      sendToPeer("LEVEL_START");
    }
  }

  void onExitLevel() { m_inLevel = false; }

  // Send jump/release to peer
  void sendJump() { sendToPeer("JUMP"); }
  void sendRelease() { sendToPeer("RELEASE"); }
  void sendP2Jump() { sendToPeer("P2JUMP"); }
  void sendP2Release() { sendToPeer("P2RELEASE"); }

private:
  NetworkManager() {}

  void receiveLoop() {
    char buffer[256];
    struct sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    while (m_running) {
      int n = recvfrom(m_socket, buffer, sizeof(buffer) - 1, 0,
                       (struct sockaddr *)&fromAddr, &fromLen);
      if (n > 0) {
        buffer[n] = '\0';
        std::string msg(buffer);

        // If host: learn peer address from first message
        if (m_isHost && !m_peerKnown) {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_peerAddr = fromAddr;
          m_peerKnown = true;
          sendToPeer("WELCOME");
          log::info("Peer connected!");
        }

        handleMessage(msg);
      }
    }
  }

  void handleMessage(const std::string &msg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (msg == "HELLO") {
      // Guest said hello, already handled above (peer address learned)
    } else if (msg == "WELCOME") {
      log::info("Connected to host!");
    } else if (msg == "LEVEL_START") {
      m_activePlayer = 1;
      m_inLevel = true;
      m_swapJustHappened = false;
      m_warningSeconds = 0;
    } else if (msg.find("SWAP ") == 0) {
      m_activePlayer = std::stoi(msg.substr(5));
      m_swapJustHappened = true;
      m_warningSeconds = 0;
      m_showingWarning = false;
    } else if (msg.find("WARNING ") == 0) {
      m_warningSeconds = std::stoi(msg.substr(8));
      m_showingWarning = true;
    } else if (msg == "JUMP") {
      m_remoteJump = true;
    } else if (msg == "RELEASE") {
      m_remoteRelease = true;
    } else if (msg == "P2JUMP") {
      m_remoteP2Jump = true;
    } else if (msg == "P2RELEASE") {
      m_remoteP2Release = true;
    }
  }
};

// ─────────────────────────────────────────
// Swap Connect Popup (Host / Join UI)
// ─────────────────────────────────────────

class SwapConnectPopup : public FLAlertLayer {
protected:
  CCTextInputNode *m_ipInput = nullptr;
  CCLabelBMFont *m_statusLabel = nullptr;

  bool init() {
    if (!FLAlertLayer::init(75))
      return false;

    auto winSize = CCDirector::sharedDirector()->getWinSize();
    log::info("SwapConnectPopup::init - winSize: {}x{}", winSize.width,
              winSize.height);

    // Dark background
    m_noElasticity = true;
    auto bg = CCScale9Sprite::create("GJ_square01.png", {0, 0, 80, 80});
    bg->setContentSize({300.f, 200.f});
    bg->setPosition(winSize / 2);
    m_mainLayer->addChild(bg);

    // Title
    auto title = CCLabelBMFont::create("GD Swap Mode", "goldFont.fnt");
    title->setScale(0.7f);
    title->setPosition(ccp(winSize.width / 2, winSize.height / 2 + 75));
    m_mainLayer->addChild(title);

    auto menu = CCMenu::create();
    menu->setPosition(ccp(0, 0));
    m_mainLayer->addChild(menu, 10);

    // Set menu touch priority higher than FLAlertLayer's
    menu->setTouchPriority(-502);

    // Close button
    auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
    auto closeBtn = CCMenuItemSpriteExtra::create(
        closeSpr, this, menu_selector(SwapConnectPopup::onClose));
    closeBtn->setPosition(
        ccp(winSize.width / 2 - 145, winSize.height / 2 + 95));
    menu->addChild(closeBtn);

    // IP input label
    auto ipLabel =
        CCLabelBMFont::create("IP kolegi (Radmin VPN):", "bigFont.fnt");
    ipLabel->setScale(0.35f);
    ipLabel->setPosition(ccp(winSize.width / 2, winSize.height / 2 + 40));
    m_mainLayer->addChild(ipLabel);

    // IP text input
    m_ipInput =
        CCTextInputNode::create(200.f, 30.f, "np. 26.0.0.1", "bigFont.fnt");
    m_ipInput->setPosition(ccp(winSize.width / 2, winSize.height / 2 + 15));
    m_ipInput->setMaxLabelScale(0.55f);
    m_mainLayer->addChild(m_ipInput);

    // Status label
    m_statusLabel = CCLabelBMFont::create("Nie polaczono", "bigFont.fnt");
    m_statusLabel->setScale(0.3f);
    m_statusLabel->setColor(ccc3(200, 200, 200));
    m_statusLabel->setPosition(ccp(winSize.width / 2, winSize.height / 2 - 10));
    m_mainLayer->addChild(m_statusLabel);

    // HOST button
    auto hostSpr = ButtonSprite::create("Hostuj", "goldFont.fnt",
                                        "GJ_button_01.png", 0.8f);
    auto hostBtn = CCMenuItemSpriteExtra::create(
        hostSpr, this, menu_selector(SwapConnectPopup::onHost));
    hostBtn->setPosition(ccp(winSize.width / 2 - 70, winSize.height / 2 - 45));
    menu->addChild(hostBtn);

    // JOIN button
    auto joinSpr = ButtonSprite::create("Dolacz", "goldFont.fnt",
                                        "GJ_button_02.png", 0.8f);
    auto joinBtn = CCMenuItemSpriteExtra::create(
        joinSpr, this, menu_selector(SwapConnectPopup::onJoin));
    joinBtn->setPosition(ccp(winSize.width / 2 + 70, winSize.height / 2 - 45));
    menu->addChild(joinBtn);

    // DISCONNECT button
    auto dcSpr = ButtonSprite::create("Rozlacz", "goldFont.fnt",
                                      "GJ_button_06.png", 0.7f);
    auto dcBtn = CCMenuItemSpriteExtra::create(
        dcSpr, this, menu_selector(SwapConnectPopup::onDisconnect));
    dcBtn->setPosition(ccp(winSize.width / 2, winSize.height / 2 - 80));
    menu->addChild(dcBtn);

    // Schedule status check
    this->schedule(schedule_selector(SwapConnectPopup::checkStatus), 0.5f);

    this->setKeypadEnabled(true);
    this->setTouchEnabled(true);
    this->registerWithTouchDispatcher();

    return true;
  }

  void onClose(CCObject *) {
    this->setKeypadEnabled(false);
    this->removeFromParentAndCleanup(true);
  }

  void keyBackClicked() override { onClose(nullptr); }

  void onHost(CCObject *) {
    log::info("onHost clicked!");
    auto net = NetworkManager::get();
    if (net->m_connected) {
      log::info("Already connected");
      m_statusLabel->setString("Juz polaczony!");
      return;
    }

    if (net->hostSession()) {
      log::info("Host session started successfully");
      m_statusLabel->setString("Hostujesz! Czekam na kolege...");
      m_statusLabel->setColor(ccc3(255, 255, 100));
    } else {
      log::info("Host session FAILED");
      m_statusLabel->setString("Blad hostowania!");
      m_statusLabel->setColor(ccc3(255, 80, 80));
    }
  }

  void onJoin(CCObject *) {
    auto net = NetworkManager::get();
    if (net->m_connected) {
      m_statusLabel->setString("Juz polaczony!");
      return;
    }

    std::string ip = m_ipInput->getString();
    if (ip.empty()) {
      m_statusLabel->setString("Wpisz IP kolegi!");
      m_statusLabel->setColor(ccc3(255, 80, 80));
      return;
    }

    if (net->joinSession(ip)) {
      m_statusLabel->setString("Laczenie...");
      m_statusLabel->setColor(ccc3(255, 255, 100));
    } else {
      m_statusLabel->setString("Blad laczenia!");
      m_statusLabel->setColor(ccc3(255, 80, 80));
    }
  }

  void onDisconnect(CCObject *) {
    NetworkManager::get()->disconnect();
    m_statusLabel->setString("Rozlaczono");
    m_statusLabel->setColor(ccc3(200, 200, 200));
  }

  void checkStatus(float dt) {
    auto net = NetworkManager::get();
    if (!net->m_connected)
      return;

    if (net->m_isHost) {
      if (net->m_peerKnown) {
        m_statusLabel->setString("Kolega polaczony! Wejdzcie na level!");
        m_statusLabel->setColor(ccc3(100, 255, 100));
      } else {
        m_statusLabel->setString("Hostujesz - czekam na kolege...");
        m_statusLabel->setColor(ccc3(255, 255, 100));
      }
    } else {
      if (net->m_peerKnown) {
        m_statusLabel->setString("Polaczono z hostem! Wejdzcie na level!");
        m_statusLabel->setColor(ccc3(100, 255, 100));
      }
    }
  }

public:
  static SwapConnectPopup *create() {
    auto ret = new SwapConnectPopup();
    if (ret->init()) {
      ret->autorelease();
      return ret;
    }
    delete ret;
    return nullptr;
  }

  void show() {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    scene->addChild(this, 100);
  }
};

// ─────────────────────────────────────────
// Menu Hook — SWAP button at bottom
// ─────────────────────────────────────────

class $modify(SwapMenuLayer, MenuLayer) {
  bool init() {
    if (!MenuLayer::init())
      return false;

    auto winSize = CCDirector::sharedDirector()->getWinSize();

    auto label = CCLabelBMFont::create("SWAP", "bigFont.fnt");
    label->setScale(0.45f);

    auto sprite = CircleButtonSprite::create(label, CircleBaseColor::Green,
                                             CircleBaseSize::Medium);

    auto btn = CCMenuItemSpriteExtra::create(
        sprite, this, menu_selector(SwapMenuLayer::onSwapBtn));

    auto menu = CCMenu::create();
    menu->addChild(btn);
    menu->setPosition(ccp(winSize.width / 2, 45.f));
    this->addChild(menu, 10);

    return true;
  }

  void onSwapBtn(CCObject *) { SwapConnectPopup::create()->show(); }
};

// ─────────────────────────────────────────
// PlayLayer Hook — swap logic & HUD
// ─────────────────────────────────────────

class $modify(SwapPlayLayer, PlayLayer) {
  struct Fields {
    CCLabelBMFont *m_swapLabel = nullptr;
    CCLabelBMFont *m_modeLabel = nullptr;
    float m_swapDisplayTimer = 0.f;
    int m_lastWarning = 0;
  };

  bool init(GJGameLevel *level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects))
      return false;

    auto net = NetworkManager::get();
    if (!net->m_connected || !net->m_peerKnown)
      return true;

    auto winSize = CCDirector::sharedDirector()->getWinSize();

    // Warning / swap label (top-right corner)
    m_fields->m_swapLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_fields->m_swapLabel->setPosition(
        ccp(winSize.width - 90, winSize.height - 25));
    m_fields->m_swapLabel->setScale(0.45f);
    m_fields->m_swapLabel->setOpacity(0);
    m_fields->m_swapLabel->setZOrder(1000);
    this->addChild(m_fields->m_swapLabel);

    // Mode label (top-left corner)
    m_fields->m_modeLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_fields->m_modeLabel->setPosition(ccp(90, winSize.height - 25));
    m_fields->m_modeLabel->setScale(0.4f);
    m_fields->m_modeLabel->setZOrder(1000);
    this->addChild(m_fields->m_modeLabel);

    // Notify network that we entered a level
    net->onEnterLevel();
    updateModeLabel();

    return true;
  }

  void onQuit() {
    NetworkManager::get()->onExitLevel();
    PlayLayer::onQuit();
  }

  void update(float dt) {
    PlayLayer::update(dt);

    auto net = NetworkManager::get();
    if (!net->m_connected || !net->m_peerKnown)
      return;

    // Host manages the swap timer
    net->updateSwapTimer(dt);

    // Handle swap event
    if (net->m_swapJustHappened) {
      net->m_swapJustHappened = false;
      m_fields->m_swapDisplayTimer = 2.5f;
      m_fields->m_lastWarning = 0;

      if (m_fields->m_swapLabel) {
        m_fields->m_swapLabel->setString("SWAP!");
        m_fields->m_swapLabel->setOpacity(255);
        m_fields->m_swapLabel->setColor(ccc3(255, 50, 50));
        m_fields->m_swapLabel->setScale(0.7f);
        m_fields->m_swapLabel->stopAllActions();
        m_fields->m_swapLabel->runAction(CCScaleTo::create(0.3f, 0.45f));
      }
      updateModeLabel();
    }

    // Handle warning countdown
    if (net->m_warningSeconds > 0 &&
        net->m_warningSeconds != m_fields->m_lastWarning) {
      m_fields->m_lastWarning = net->m_warningSeconds;
      m_fields->m_swapDisplayTimer = 4.f;

      if (m_fields->m_swapLabel) {
        auto text = fmt::format("Zmiana za {}...", net->m_warningSeconds);
        m_fields->m_swapLabel->setString(text.c_str());
        m_fields->m_swapLabel->setOpacity(255);
        m_fields->m_swapLabel->setColor(ccc3(255, 255, 50));
        m_fields->m_swapLabel->setScale(0.5f);
      }
    }

    // Fade out swap label after display time
    if (m_fields->m_swapDisplayTimer > 0.f) {
      m_fields->m_swapDisplayTimer -= dt;
      if (m_fields->m_swapDisplayTimer <= 0.f && net->m_warningSeconds == 0) {
        if (m_fields->m_swapLabel) {
          m_fields->m_swapLabel->stopAllActions();
          m_fields->m_swapLabel->runAction(CCFadeOut::create(0.5f));
        }
      }
    }

    // Apply remote inputs (when WE are NOT the active player, remote inputs
    // move OUR player object)
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

  void updateModeLabel() {
    auto net = NetworkManager::get();
    if (!m_fields->m_modeLabel)
      return;

    if (net->isActivePlayer()) {
      m_fields->m_modeLabel->setString("TY GRASZ!");
      m_fields->m_modeLabel->setColor(ccc3(100, 255, 100));
    } else {
      m_fields->m_modeLabel->setString("OGLADASZ");
      m_fields->m_modeLabel->setColor(ccc3(255, 100, 100));
    }
  }
};

// ─────────────────────────────────────────
// Input Hook — intercept & relay inputs
// ─────────────────────────────────────────

class $modify(SwapBaseGameLayer, GJBaseGameLayer) {
  void handleButton(bool push, int button, bool player1) {
    auto net = NetworkManager::get();

    // No swap session active — pass through normally
    if (!net->m_connected || !net->m_peerKnown || !net->m_inLevel) {
      GJBaseGameLayer::handleButton(push, button, player1);
      return;
    }

    if (net->isActivePlayer()) {
      // WE are active: allow local input AND send to peer
      GJBaseGameLayer::handleButton(push, button, player1);

      if (player1) {
        if (push)
          net->sendJump();
        else
          net->sendRelease();
      } else {
        if (push)
          net->sendP2Jump();
        else
          net->sendP2Release();
      }
    }
    // NOT active: block local input entirely (remote inputs come via update())
  }
};
