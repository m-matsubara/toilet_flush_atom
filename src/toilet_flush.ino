/**
 * Toilet flush for ATOM  Copyright © 2022 m.matsubara
 * 
 * ## 概要
 *   配管上の問題でトイレ詰まりをよく起こすトイレにおいて、使用後に念入りに流すため、離席後一定時間たったら（離席時に流すが、タンクに水がたまるのを待つ）、再度タイマーでトイレを流す（大）。
 *   
 * ## 対象機材（トイレ）
 * * リモコンで流す動作可能なシャワートイレ (INAX プレアス DT-CL114A・CH184A用リモコンで確認)
 *   (リモコンのコマンドが異なる場合、送信コマンドを赤外線センサで解析する必要があります。)
 *
 * ## 必要機材（このプログラムを動作させるのに使用する機材）
 * * M5Atom Lite (または Matrix)
 * * M5Stack用赤外線送受信ユニット [U002] (本体の赤外線LEDが使える場合は不要・送信コマンドを赤外線センサで解析する必要がある場合は必要)
 * 
 */

// 外付け赤外線LEDを使用する時定義する（M5Stack用赤外線送受信ユニット(GROVE互換端子)）
#define USE_EXTERNAL_IR_LED

// 内蔵赤外線LEDを使用する時定義する
//#define USE_INTERNAL_IR_LED

// 流すコマンド
// INAX (プレアス DT-CL114A・CH184A)
#define FLUSH_IR_COMMAND_TYPE decode_type_t::INAX
#define FLUSH_IR_COMMAND_CODE 0x5C30CF		            // ながす（大）コマンド
//#define FLUSH_IR_COMMAND_CODE 0x5C32CD	            // ながす（小）コマンド
#define FLUSH_IR_COMMAND_BITS 24
// TOTO (機種不明)
//#define FLUSH_IR_COMMAND_TYPE decode_type_t::TOTO
//#define FLUSH_IR_COMMAND_CODE 0xD0D00		            // T ながす（大）コマンド
//#define FLUSH_IR_COMMAND_BITS 24


#include <stdlib.h>
#include <Arduino.h>
#include <M5Atom.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <Preferences.h>

// ボタン長押しの境界時間
const uint32_t BUTTON_LONG_PRESS_THRESHOLD = 3000;

// 赤外線LED接続端子
const uint16_t IR_LED_EXTERNAL = 26; // M5Stack用赤外線送受信ユニット(GROVE互換端子)
const boolean  IR_LED_EXTERNAL_INVERTED = false; // M5Stack用赤外線送受信ユニット(GROVE互換端子)は、1出力で点灯
const uint16_t IR_LED_INTERNAL = 12; // 内蔵赤外線 LED
const boolean  IR_LED_INTERNAL_INVERTED = true; // 内蔵赤外線 LED は、0出力で点灯
// 赤外線センサー接続端子
const uint16_t IR_SENSOR = 32;       // M5Stack用赤外線送受信ユニット(GROVE互換端子)


// 赤外線送信クラス
#ifdef USE_EXTERNAL_IR_LED
IRsend irsendExternal(IR_LED_EXTERNAL, IR_LED_EXTERNAL_INVERTED); // M5Stack用赤外線送受信ユニット(GROVE互換端子)
#endif
#ifdef USE_INTERNAL_IR_LED
IRsend irsendInternal(IR_LED_INTERNAL, IR_LED_INTERNAL_INVERTED); // 内蔵赤外線 LED
#endif

// 赤外線受信クラス
IRrecv irrecv(IR_SENSOR, 1024, 50, true);	// 引数は、IRrecvDumpV2 を参考にした

// 赤外線コマンド
decode_type_t irCommandType = FLUSH_IR_COMMAND_TYPE;
uint64_t irCommandCode = FLUSH_IR_COMMAND_CODE;
uint16_t irCommandBits = FLUSH_IR_COMMAND_BITS;
uint16_t *irCommandBuff = NULL;
uint16_t irCommandBuffLen = 0;

// 設定値・カウントダウンタイマー(ms)（離席後時間経過後にトイレフラッシュ）
//int32_t countdownTimer = 120000; 
int32_t countdownTimer = 120000; 



// loop処理の時刻（loop()関数の中で更新）
uint32_t timeValue = millis();

// ステータス
enum class Status {
  Waiting           // 待ち状態
  , Countdown       // カウントダウン
};
Status status = Status::Waiting;
// ステータスが変更された時刻
uint32_t timeChangeStatus = 0;

// ボタンが押下された時刻
uint32_t timeBtnPressed = 0;

// 設定
Preferences pref;

// 赤外線コマンド学習モードの時 true
boolean isIRReceiveMode = false;

// ループごとにインクリメントする
uint32_t loopCounter = 0;

/**
 * 設定を読み込む
 */ 
void loadSetting() {
  // 2022/08/20 現在、PlatformIO では platformio.ini に以下の記述がないと Preferences クラスが正しく動作しない
  // platform = https://github.com/tasmota/platform-espressif32/releases/download/v2.0.2idf/platform-espressif32-2.0.2.zip

  pref.begin("toilet_flush", false);

  // 以下は赤外線受信モードで設定される項目
  irCommandType    = (decode_type_t)pref.getInt("irCommandType", FLUSH_IR_COMMAND_TYPE);
  irCommandCode    = pref.getInt("irCommandCode", FLUSH_IR_COMMAND_CODE);
  irCommandBits    = pref.getInt("irCommandBits", FLUSH_IR_COMMAND_BITS);
  irCommandBuffLen = pref.getInt("irCommandLen", 0);
  if (irCommandBuff != NULL) {
    delete[] irCommandBuff;
    irCommandBuff = NULL;
  }
  if (irCommandBuffLen != 0) {
    irCommandBuff = new uint16_t[irCommandBuffLen];
    pref.getBytes("irCommandBuff", (void *)irCommandBuff, irCommandBuffLen * sizeof(uint16_t));
  }
  Serial.printf("  irCommandType = %d (%s)\n", irCommandType, typeToString(irCommandType).c_str());
  Serial.printf("  irCommandCode = 0x%x\n", irCommandCode);
  Serial.printf("  irCommandBits = %d\n", irCommandBits);
  Serial.printf("  irCommandBuffLen = %d\n", irCommandBuffLen);
  if (irCommandBuffLen != 0) {
    Serial.printf("  irCommandBuff[%u] = {", irCommandBuffLen);
    for (int i = 0; i < irCommandBuffLen; i++) {
      if (i == 0)
        Serial.printf("%u", irCommandBuff[i]);
      else
        Serial.printf(", %u", irCommandBuff[i]);
    }
    Serial.println("}");
  }
  pref.end();
}

/**
 * トイレフラッシュ（大）関数
 */
void flush() {
  setCpuFrequencyMhz(240);  // CPUクロックが低いままだと送信が安定しない
  irrecv.disableIRIn(); // 自身の赤外線コマンドを受信してしまったりするのでいったん無効化
#ifdef USE_EXTERNAL_IR_LED
  if (irCommandType != decode_type_t::UNKNOWN)
    irsendExternal.send(irCommandType, irCommandCode, irCommandBits);
  else {
    irsendExternal.sendRaw((const uint16_t *)irCommandBuff, irCommandBuffLen, 38);
    Serial.printf("command[%u] = {", irCommandBuffLen);
    for (int i = 0; i < irCommandBuffLen; i++) {
      if (i == 0)
        Serial.printf("%u", irCommandBuff[i]);
      else
        Serial.printf(", %u", irCommandBuff[i]);
    }
    Serial.println("}");
  }
#endif
  M5.dis.drawpix(0, CRGB(0, 0, 100));
  delay(250);
  M5.dis.drawpix(0, CRGB(100, 100, 100));
  delay(250);
#ifdef USE_INTERNAL_IR_LED
  if (irCommandType != decode_type_t::UNKNOWN)
    irsendInternal.send(irCommandType, irCommandCode, irCommandBits);
  else
    irsendInternal.sendRaw((const uint16_t *)irCommandBuff, irCommandBuffLen, 38);
#endif

  M5.dis.drawpix(0, CRGB(0, 0, 100));
  delay(250);
  M5.dis.drawpix(0, CRGB(100, 100, 100));
  delay(250);
  M5.dis.drawpix(0, CRGB(0, 0, 0));
  Serial.printf("Flush.\n");
  Serial.printf("irCommandType: %s\n", typeToString(irCommandType, false).c_str());
  Serial.printf("irCommandCode: %x\n", irCommandCode);
  Serial.printf("irCommandType: %d\n", irCommandBits);

  irrecv.enableIRIn();

  // CPU速度を戻す
  setCpuFrequencyMhz(80);	// 低すぎると、LEDが点灯しっぱなしになったりする。
}

/*
 * ステータス変更
 */
void changeStatus(Status newStatus) {
  status = newStatus;
  timeChangeStatus = timeValue;
}

/*
 * 赤外線受信モードの初期化
 */
void irRecvSetup() {
}

/**
  * 赤外線コマンド学習モードのループ処理
  */
void irRecvLoop() {
  // 赤外線受信結果
  decode_results results;
  M5.update();
  if ((loopCounter % 100 < 10))
    M5.dis.drawpix(0, CRGB(100, 100, 0));
  else
    M5.dis.drawpix(0, CRGB(0, 0, 0));

  if (irrecv.decode(&results)) {
    // 受信したコマンドを解析
    String typeName = typeToString(results.decode_type, results.repeat);
    String commandCodeStr = resultToHexidecimal(&results);
    uint64_t commandCode = strtol(commandCodeStr.c_str() , NULL, 16);
    if (results.repeat == false && results.overflow == false) {
      if (results.decode_type == decode_type_t::UNKNOWN) {
        irCommandType = results.decode_type;
        irCommandCode = commandCode;
        irCommandBits = results.bits;
        if (irCommandBuff != NULL)
          delete[] irCommandBuff;
        irCommandBuff = resultToRawArray(&results);
        irCommandBuffLen = getCorrectedRawLength(&results);

        Serial.printf("command[%u] = {", irCommandBuffLen);
        for (int i = 0; i < irCommandBuffLen; i++) {
          if (i == 0)
            Serial.printf("%u", irCommandBuff[i]);
          else
            Serial.printf(", %u", irCommandBuff[i]);
        }
        Serial.println("}");
      } else if (commandCode != 0) {
        irCommandType = results.decode_type;
        irCommandCode = commandCode;
        irCommandBits = results.bits;

        // UNKNOWNでない場合は、コマンドバッファは削除（保存もしない）
        if (irCommandBuff != NULL)
          delete[] irCommandBuff;
        irCommandBuff = NULL;
        irCommandBuffLen = 0;
      }
    }
    M5.dis.drawpix(0, CRGB(100, 100, 0));
    delay(2000);
    M5.dis.drawpix(0, CRGB(0, 0, 0));
    irrecv.resume();
  }

  if (M5.Btn.wasPressed()) {
    timeBtnPressed = timeValue;
    flush();
  }
  if ((M5.Btn.read() != 0) and (timeValue - timeBtnPressed >= BUTTON_LONG_PRESS_THRESHOLD)) {
    // コマンド保存
    pref.begin("toilet_flush", false);
    pref.putInt("irCommandType", irCommandType);
    pref.putInt("irCommandCode", irCommandCode);
    pref.putInt("irCommandBits", irCommandBits);
    pref.end();
    isIRReceiveMode = false;
    timeBtnPressed = timeValue;
    Serial.println("Normal mode.");
  }
  delay(10);
}


void setup() {
  // CPU速度を80Mhzに変更(ディスプレイの初期化の後に実行するとディスプレイがちらつく)
  // 40MHz 以下に落とすとディスプレイが全灯になってしまう。（うまく通信できない？）
  setCpuFrequencyMhz(80);

  // M5初期化
  M5.begin(true, false, true);

  Serial.println("toilet_flush(atom)");
  M5.dis.setBrightness(20);

  // LED 1秒点灯
  M5.dis.drawpix(0, CRGB(0, 100, 0));
  delay(1000);
  M5.dis.drawpix(0, CRGB(0, 0, 0));

  // 設定値の読み込み
  loadSetting();

  if (M5.Btn.isPressed() || (irCommandType == decode_type_t::UNKNOWN)) {
    // 赤外線コマンド学習モード
    isIRReceiveMode = true;
    Serial.println("Receive mode");
  } else {
    Serial.println("Normal mode.");
  }

  // 外付け赤外線LEDの初期化
#ifdef USE_EXTERNAL_IR_LED
  irsendExternal.begin();
#endif

  // 内蔵赤外線LEDの初期化
#ifdef USE_INTERNAL_IR_LED
  irsendInternal.begin();
#endif

  // 赤外線受信のための定数設定(IRrecvDumpV2より取得)
  irrecv.setUnknownThreshold(12); // この値より短いON/OFFの値を無視する閾値
  irrecv.setTolerance(25);        // 許容範囲
  // 受信開始
  irrecv.enableIRIn();

  changeStatus(Status::Waiting);
  if (isIRReceiveMode == false) {
  } else {
    // 赤外線受信モード
    irRecvSetup();
  }
}

/**
  * 通常モードのループ
  */
void normalLoop() {
  M5.update();
  if ((status == Status::Countdown) && ((loopCounter % 100 < 10) || (timeValue - timeChangeStatus >= countdownTimer - 10000)))
    M5.dis.drawpix(0, CRGB(0, 100, 0));
  else
    M5.dis.drawpix(0, CRGB(0, 0, 0));

  // 赤外線受信結果
  decode_results results;
  if (irrecv.decode(&results)) {
    // 受信したコマンドを解析
    String typeName = typeToString(results.decode_type, results.repeat);
    String commandCodeStr = resultToHexidecimal(&results);
    uint64_t commandCode = strtol(commandCodeStr.c_str() , NULL, 16);
    if ((irCommandType == results.decode_type) && (irCommandCode == commandCode) && (irCommandBits = results.bits)) {
      changeStatus(Status::Countdown);
    }
  }
  if (status == Status::Countdown) {
    if (timeValue - timeChangeStatus >= countdownTimer) {
      flush();
      M5.dis.drawpix(0, CRGB(0, 0, 0));
      changeStatus(Status::Waiting);
    }  
  }
  if (M5.Btn.wasPressed()) {
    if (status == Status::Countdown)
      changeStatus(Status::Waiting);
    else 
      changeStatus(Status::Countdown);
    timeBtnPressed = timeValue;
  }
  // ボタン長押しでフラッシュ
  if ((M5.Btn.read() != 0) and (timeValue - timeBtnPressed >= BUTTON_LONG_PRESS_THRESHOLD)) {
    flush();
    changeStatus(Status::Waiting);
    timeBtnPressed = timeValue;
  }
  delay(10);
}


void loop() {
  // 処理時刻の更新
  timeValue = millis();

  if (isIRReceiveMode)
    irRecvLoop();
  else
    normalLoop();
  loopCounter++;
}