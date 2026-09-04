// FIREBASE
#ifndef BUTTON_MAPPER

#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "EInkDisplay.h"
#include <vector>                      // 引入動態陣列庫
#include "hardware/BoardSupport.h"     // 引入原廠的實體按鈕驅動

// 提供 Firebase 授權與字串處理的輔助工具
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#include "../secrets.h" // for API_KEY DATABASE_URL, WIFI_SSID and WIFI_PASSWORD


// ==========================================
// 分頁系統全域變數
// ==========================================
std::vector<String> globalLines; // 儲存切割好的每一行文字
int currentPage = 0;             // 目前所在的頁碼 (0 代表第一頁)
const int MAX_LINES_PER_PAGE = 8; // ⚠️ 每頁最多顯示幾行

bool isEmergencyMode = false;      // 是否處於文字觸發的緊急狀態
unsigned long lastScreenFlash = 0; // 記錄上次螢幕刷新的時間


// ==========================================
// 1. WiFi 設定
// ==========================================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;


FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;
EInkDisplay einkDisplay;


String wrapText(const String& text, int maxLineUnits = 45) { // 這裡的數字你可以繼續微調
  String result = "";
  String currentWord = "";
  int currentLineUnits = 0;
  int currentWordUnits = 0;

  for (unsigned int i = 0; i < text.length(); ) {
    char c = text[i];

    // 1. 處理手動換行符號
    if (c == '\n') {
      if (currentWord.length() > 0) {
        if (currentLineUnits + currentWordUnits > maxLineUnits && currentLineUnits > 0) {
          result += '\n';
        }
        result += currentWord;
        currentWord = "";
        currentWordUnits = 0;
      }
      result += '\n';
      currentLineUnits = 0;
      i++;
      continue;
    }

    // 2. 判斷字元長度與寬度
    int charBytes = 1;
    int displayWidth = 1;
    bool isASCII = true;

    if ((c & 0x80) == 0) { charBytes = 1; displayWidth = 1; isASCII = true; }         // 英文/數字 (1 byte)
    else if ((c & 0xE0) == 0xC0) { charBytes = 2; displayWidth = 1; isASCII = false; } 
    else if ((c & 0xF0) == 0xE0) { charBytes = 3; displayWidth = 2; isASCII = false; } // 中文 (寬度 2)
    else if ((c & 0xF8) == 0xF0) { charBytes = 4; displayWidth = 2; isASCII = false; } // Emoji

    if (i + charBytes > text.length()) charBytes = text.length() - i;
    String charStr = text.substring(i, i + charBytes);

    // 3. 邏輯分流：收集單字 vs 結算寬度
    if (isASCII && c != ' ') {
      // 如果是連續的英文字母或數字，把它們黏在一起收集
      currentWord += charStr;
      currentWordUnits += displayWidth;
    } else {
      // 遇到「空格」或「中文字」時，代表一個單字結束了，準備結算並把單字印出來
      if (currentWord.length() > 0) {
        // 如果加上這個單字會超出邊界，而且這行已經有字了，就把整個單字擠到下一行
        if (currentLineUnits + currentWordUnits > maxLineUnits && currentLineUnits > 0) {
          result += '\n';
          currentLineUnits = 0;
        }
        result += currentWord;
        currentLineUnits += currentWordUnits;
        currentWord = "";
        currentWordUnits = 0;
      }

      // 接著處理剛才觸發結算的「空格」或「中文字」
      if (c == ' ') {
        // 不要在新的一行開頭印出空格
        if (currentLineUnits > 0) {
          if (currentLineUnits + 1 > maxLineUnits) {
            result += '\n';
            currentLineUnits = 0;
          } else {
            result += " ";
            currentLineUnits += 1;
          }
        }
      } else {
        // 印出中文字，同樣檢查是否需要換行
        if (currentLineUnits + displayWidth > maxLineUnits && currentLineUnits > 0) {
          result += '\n';
          currentLineUnits = 0;
        }
        result += charStr;
        currentLineUnits += displayWidth;
      }
    }
    i += charBytes;
  }

  // 4. 把最後殘留的單字印出來
  if (currentWord.length() > 0) {
    if (currentLineUnits + currentWordUnits > maxLineUnits && currentLineUnits > 0) {
      result += '\n';
    }
    result += currentWord;
  }

  return result;
}

void renderPage(bool showEmergencyHeader = false) {
  if (globalLines.empty()) return;
  
  String pageText = "";

  if (showEmergencyHeader) {
    pageText += " *** EMERGENCY *** \n";
  } else {
    pageText += "\n"; 
  }

  int totalPages = (globalLines.size() + MAX_LINES_PER_PAGE - 1) / MAX_LINES_PER_PAGE;
  
  int startIdx = currentPage * MAX_LINES_PER_PAGE;
  int endIdx = startIdx + MAX_LINES_PER_PAGE;
  if (endIdx > globalLines.size()) {
    endIdx = globalLines.size();
  }

  // 計算這一頁實際有幾行文字
  int actualLines = endIdx - startIdx;

  // 1. 組合這頁真正的文字
  for (int i = startIdx; i < endIdx; i++) {
    pageText += globalLines[i];
    pageText += "\n";
  }

  // ⚠️ 2. 關鍵修正：如果這頁文字不夠，就狂加換行符號，把空間佔滿
  for (int i = actualLines; i < MAX_LINES_PER_PAGE; i++) {
    pageText += "\n";
  }

  // 3. 在最底下印出頁數 (不需要前面的 \n 了，因為上面已經補齊)
  if (totalPages > 1) {
    pageText += "(Page ";
    pageText += String(currentPage + 1);
    pageText += "/";
    pageText += String(totalPages);
    pageText += ")";
  }

  // 交給墨水屏畫出來
  einkDisplay.showWifiStatus(pageText);
}


void streamCallback(FirebaseStream data) {
  if (data.dataType() == "string") {
    String message = data.stringData();
    message.replace("\\n", "\n"); 

    String lowerMsg = message;
    lowerMsg.toLowerCase();
    
    if (lowerMsg.indexOf("mayday") >= 0) {
      isEmergencyMode = true;
      Serial.println("\n[警報] 偵測到 Mayday 關鍵字，啟動螢幕閃爍！");
    } else {
      isEmergencyMode = false;
      Serial.println("\n[Firebase] 收到一般推送，解除警報。");
    }
    
    // 呼叫你的自動換行函數
    String wrappedMessage = wrapText(message, 45); 

    // 清空舊的文章，準備存入新的文章
    globalLines.clear();
    int startIndex = 0;
    
    // 根據 \n 換行符號，把文章切成一行一行存進陣列裡
    for (int i = 0; i < wrappedMessage.length(); i++) {
      if (wrappedMessage[i] == '\n') {
        globalLines.push_back(wrappedMessage.substring(startIndex, i));
        startIndex = i + 1;
      }
    }
    if (startIndex < wrappedMessage.length()) {
      globalLines.push_back(wrappedMessage.substring(startIndex));
    }
    
    Serial.print("\n[Firebase] 收到雲端即時推送，共 ");
    Serial.print(String(globalLines.size()));
    Serial.println(" 行");
    
    // 每次收到新訊息，都強制回到第一頁並刷新畫面
    currentPage = 0;
    renderPage();
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[Firebase] 串流連線逾時，系統將自動重連...");
}

void setup() {
  Serial.begin(115200);
  delay(2000);


  transitink::hardware::configureButtons();
  transitink::hardware::startButtonMonitoring();


  einkDisplay.begin(false);
  einkDisplay.showWifiStatus("Connecting to WiFi...");

  // 連接 Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 初始化 Firebase 設定
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  // 使用匿名登入 (確保你在 Firebase 後台有開啟匿名登入)
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase 授權成功");
  } else {
    Serial.printf("Firebase 授權失敗: %s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // 建立即時串流 (Stream)，監聽資料庫中的 /zectrix/text 節點
  if (!Firebase.RTDB.beginStream(&stream, "/zectrix/text")) {
    Serial.printf("串流建立失敗: %s\n", stream.errorReason().c_str());
  }
  
  // 綁定剛才寫好的 Callback 函數
  Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);

  einkDisplay.showWifiStatus("Firebase 即時同步啟動\n等待雲端推送...");
}

void loop() {
  if (Firebase.ready()) {
     // 系統閒置時的背景處理
  }

  if (isEmergencyMode) {
    // 墨水屏刷新需要一點時間，1.5 秒是很好的節奏
    if (millis() - lastScreenFlash >= 1500) {
      lastScreenFlash = millis();
      
      // 交替變數
      static bool toggleFlash = false;
      toggleFlash = !toggleFlash;
      
      // 呼叫渲染函數，並決定這一次要不要畫出 EMERGENCY
      renderPage(toggleFlash);
    }
  }

  // ==========================================
  // 實體按鈕監聽與翻頁邏輯
  // ==========================================
  static bool lastUp = false;
  static bool lastDown = false;
  // static bool lastHome = false;
  
  // 讀取上下按鈕的狀態
  bool upPressed = transitink::hardware::factoryResetUpButtonPressed();
  bool downPressed = transitink::hardware::factoryResetDownButtonPressed();
  // bool homePressed = transitink::hardware::homeButtonPressed();

  // 偵測「上」按鈕被按下 (且不是一直按著不放)
  if (upPressed && !lastUp) {
    if (currentPage > 0) {
      Serial.println("上一頁");
      currentPage--;
      renderPage();
    }
  }
  
  // 偵測「下」按鈕被按下
  if (downPressed && !lastDown) {
    int totalPages = (globalLines.size() + MAX_LINES_PER_PAGE - 1) / MAX_LINES_PER_PAGE;
    if (currentPage < totalPages - 1) {
      Serial.println("下一頁");
      currentPage++;
      renderPage();
    }
  }



  // 記錄這次的按鈕狀態，供下次迴圈比對
  lastUp = upPressed;
  lastDown = downPressed;
  // lastHome = homePressed;

  delay(10);
}

#endif // BUTTON_MAPPER