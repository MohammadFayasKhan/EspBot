/*
 * ============================================================================
 *  EspBot — Production-Grade ESP8266 WiFi Controlled Robot
 * ============================================================================
 *
 *  Hardware:
 *    - ESP8266 NodeMCU (Access Point mode, no router/internet required)
 *    - L293D dual H-bridge motor driver board (direct GPIO, no shift register)
 *    - 2x TT DC gear motors
 *    - 1x external LED
 *    - 4x AA battery pack (single supply: powers L293D board logic + motors)
 *
 *  Pin Map (bare L293D breakout board):
 *    IN1  -> D1 (GPIO5)   Motor 1 direction A
 *    IN2  -> D2 (GPIO4)   Motor 1 direction B
 *    IN3  -> D6 (GPIO12)  Motor 2 direction A
 *    IN4  -> D7 (GPIO13)  Motor 2 direction B
 *    LED  -> D0 (GPIO16)  External LED
 *
 *    EN1 / EN2 -> jumper caps installed on-board (always enabled, no PWM)
 *    Battery + / -  -> board's top power terminal (shared logic + motor supply)
 *    GND -> common with NodeMCU GND
 *
 *  Network:
 *    Primary: attempts to join STA SSID "Fayas" first
 *    Fallback AP SSID: EspBot_AP   PASS: 12345678   IP: 192.168.9.1
 *
 *  Architecture:
 *    The ESP8266 hosts a single-page futuristic dark dashboard, served entirely
 *    from flash (PROGMEM). The browser drives the robot via async fetch() calls
 *    to lightweight REST-style routes. The interface is accessible at the
 *    active IP (STA or AP) or via mDNS at http://espbot.local.
 *    A server-side watchdog guarantees the robot stops if the client goes
 *    silent (page closed, WiFi dropped, tab backgrounded, etc).
 *
 *  Author: Fayas | github.com/MohammadFayasKhan
 * ============================================================================
 */

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

// ============================================================================
//  SECTION 1: CONFIGURATION
// ============================================================================

namespace Config {
constexpr const char *STA_SSID = "Fayas";
constexpr const char *STA_PASSWORD = "777888666";

constexpr const char *AP_SSID = "EspBot_AP";
constexpr const char *AP_PASSWORD = "12345678";
constexpr uint8_t WIFI_CHANNEL = 6;
constexpr uint8_t MAX_CLIENTS = 4;

// Robot auto-stops if no command is received within this window.
// This is the core failsafe: browser crash, WiFi drop, or tab close
// will silently stop sending heartbeats, and the robot halts itself.
constexpr unsigned long COMMAND_TIMEOUT_MS = 600;

constexpr uint16_t HTTP_PORT = 80;
} // namespace Config

// ============================================================================
//  SECTION 2: PIN DEFINITIONS
// ============================================================================

namespace Pins {
constexpr uint8_t MOTOR1_IN1 = D1; // GPIO5  -> L293D IN1 (Motor 1 dir A)
constexpr uint8_t MOTOR1_IN2 = D2; // GPIO4  -> L293D IN2 (Motor 1 dir B)
constexpr uint8_t MOTOR2_IN3 = D6; // GPIO12 -> L293D IN3 (Motor 2 dir A)
constexpr uint8_t MOTOR2_IN4 = D7; // GPIO13 -> L293D IN4 (Motor 2 dir B)
constexpr uint8_t STATUS_LED = D0; // GPIO16 -> External LED
} // namespace Pins

// ============================================================================
//  SECTION 3: GLOBAL STATE
// ============================================================================

ESP8266WebServer server(Config::HTTP_PORT);

enum class RobotCommand : uint8_t { STOPPED, FORWARD, BACKWARD, LEFT, RIGHT };

struct RobotState {
  RobotCommand currentCommand = RobotCommand::STOPPED;
  bool ledOn = false;
  unsigned long lastCommandAt = 0;
} state;

const char *commandToString(RobotCommand cmd) {
  switch (cmd) {
  case RobotCommand::FORWARD:
    return "FORWARD";
  case RobotCommand::BACKWARD:
    return "BACKWARD";
  case RobotCommand::LEFT:
    return "LEFT";
  case RobotCommand::RIGHT:
    return "RIGHT";
  default:
    return "STOPPED";
  }
}

// ============================================================================
//  SECTION 4: LOW-LEVEL MOTOR PIN CONTROL
// ============================================================================
//
//  Direct GPIO writes to the L293D board's 4 input pins. No shift register,
//  no latch timing, no OE line — each pin is driven straight from the
//  ESP8266, exactly like a standard L293D breakout is meant to be used.

void writeMotor1(bool a, bool b) {
  digitalWrite(Pins::MOTOR1_IN1, a ? HIGH : LOW);
  digitalWrite(Pins::MOTOR1_IN2, b ? HIGH : LOW);
}

void writeMotor2(bool a, bool b) {
  digitalWrite(Pins::MOTOR2_IN3, a ? HIGH : LOW);
  digitalWrite(Pins::MOTOR2_IN4, b ? HIGH : LOW);
}

// ============================================================================
//  SECTION 5: MOTOR FUNCTIONS
// ============================================================================

void stopRobot() {
  writeMotor1(false, false);
  writeMotor2(false, false);
  state.currentCommand = RobotCommand::STOPPED;
}

void moveForward() {
  writeMotor1(false, true);
  writeMotor2(false, true);
  state.currentCommand = RobotCommand::FORWARD;
}

void moveBackward() {
  writeMotor1(true, false);
  writeMotor2(true, false);
  state.currentCommand = RobotCommand::BACKWARD;
}

// Tank-style pivot turns: wheels spin in opposite directions.
void turnLeft() {
  writeMotor1(false, true);
  writeMotor2(true, false);
  state.currentCommand = RobotCommand::LEFT;
}

void turnRight() {
  writeMotor1(true, false);
  writeMotor2(false, true);
  state.currentCommand = RobotCommand::RIGHT;
}

// ============================================================================
//  SECTION 5B: LED FUNCTIONS
// ============================================================================

void setLed(bool on) {
  state.ledOn = on;
  digitalWrite(Pins::STATUS_LED, on ? HIGH : LOW);
}

void toggleLed() { setLed(!state.ledOn); }

// ============================================================================
//  SECTION 6: WATCHDOG (FAILSAFE)
// ============================================================================

void touchWatchdog() { state.lastCommandAt = millis(); }

void serviceWatchdog() {
  if (state.currentCommand == RobotCommand::STOPPED)
    return;

  if (millis() - state.lastCommandAt > Config::COMMAND_TIMEOUT_MS) {
    stopRobot();
  }
}

// ============================================================================
//  SECTION 6B: HARDWARE DIAGNOSTICS (manual per-pin control for wiring tests)
// ============================================================================
//
//  These routes bypass moveForward()/turnLeft()/etc and drive each L293D
//  input pin directly. Use them to isolate wiring faults on a stand, with
//  motors free-spinning (no wheels touching ground).
//
//  IMPORTANT: these do NOT touch the watchdog, so a motor left running via
//  a /diag route will keep running until you hit /diag/all/off or /stop.

void diagSetPin(uint8_t pin, bool high, const char *label) {
  digitalWrite(pin, high ? HIGH : LOW);
  Serial.print(F("[DIAG] "));
  Serial.print(label);
  Serial.println(high ? F(" -> HIGH") : F(" -> LOW"));
  server.send(200, "text/plain", String(label) + (high ? " HIGH" : " LOW"));
}

void handleDiagIn1High() { diagSetPin(Pins::MOTOR1_IN1, true, "IN1"); }
void handleDiagIn1Low() { diagSetPin(Pins::MOTOR1_IN1, false, "IN1"); }
void handleDiagIn2High() { diagSetPin(Pins::MOTOR1_IN2, true, "IN2"); }
void handleDiagIn2Low() { diagSetPin(Pins::MOTOR1_IN2, false, "IN2"); }
void handleDiagIn3High() { diagSetPin(Pins::MOTOR2_IN3, true, "IN3"); }
void handleDiagIn3Low() { diagSetPin(Pins::MOTOR2_IN3, false, "IN3"); }
void handleDiagIn4High() { diagSetPin(Pins::MOTOR2_IN4, true, "IN4"); }
void handleDiagIn4Low() { diagSetPin(Pins::MOTOR2_IN4, false, "IN4"); }

void handleDiagAllOff() {
  stopRobot();
  server.send(200, "text/plain", "ALL PINS LOW");
}

void registerDiagRoutes() {
  server.on("/diag/in1/high", HTTP_GET, handleDiagIn1High);
  server.on("/diag/in1/low", HTTP_GET, handleDiagIn1Low);
  server.on("/diag/in2/high", HTTP_GET, handleDiagIn2High);
  server.on("/diag/in2/low", HTTP_GET, handleDiagIn2Low);
  server.on("/diag/in3/high", HTTP_GET, handleDiagIn3High);
  server.on("/diag/in3/low", HTTP_GET, handleDiagIn3Low);
  server.on("/diag/in4/high", HTTP_GET, handleDiagIn4High);
  server.on("/diag/in4/low", HTTP_GET, handleDiagIn4Low);
  server.on("/diag/all/off", HTTP_GET, handleDiagAllOff);
}

// ============================================================================
//  SECTION 7: EMBEDDED WEB INTERFACE (HTML + CSS + JS, PROGMEM)
// ============================================================================

const char PAGE_INDEX[] PROGMEM = R"HTML_PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#150529">
<title>EspBot</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><rect width=%22100%22 height=%22100%22 rx=%2222%22 fill=%22url(%23g)%22/><defs><linearGradient id=%22g%22 x1=%220%22 y1=%220%22 x2=%22100%22 y2=%22100%22><stop offset=%220%25%22 stop-color=%22%238b5cf6%22/><stop offset=%22100%25%22 stop-color=%22%23a855f7%22/></linearGradient></defs><path d=%22M73 24H27v52h46M27 50h36%22 stroke=%22white%22 stroke-width=%2212%22 stroke-linecap=%22round%22 stroke-linejoin=%22round%22 fill=%22none%22/></svg>">
<link rel="apple-touch-icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><rect width=%22100%22 height=%22100%22 rx=%2222%22 fill=%22url(%23g)%22/><defs><linearGradient id=%22g%22 x1=%220%22 y1=%220%22 x2=%22100%22 y2=%22100%22><stop offset=%220%25%22 stop-color=%22%238b5cf6%22/><stop offset=%22100%25%22 stop-color=%22%23a855f7%22/></linearGradient></defs><path d=%22M73 24H27v52h46M27 50h36%22 stroke=%22white%22 stroke-width=%2212%22 stroke-linecap=%22round%22 stroke-linejoin=%22round%22 fill=%22none%22/></svg>">
<style>
:root{
  --bg:#150529;
  --surface:#0d0d0f;
  --surface-solid:#0d0d0f;
  --surface-elevated:#141116;
  --border:#27272a;
  --border-light:rgba(168,85,247,0.25);
  --blue:#8b5cf6;
  --green:#a855f7;
  --red:#f43f5e;
  --orange:#c084fc;
  --purple:#a855f7;
  --text:#ffffff;
  --text2:#d1d5db;
  --text3:#9ca3af;
  --sans:Inter,-apple-system,BlinkMacSystemFont,"SF Pro Display","SF Pro Text","Helvetica Neue",Helvetica,system-ui,sans-serif;
  --mono:"SF Mono","SFMono-Regular",Menlo,Consolas,monospace;
  --radius:20px;
  --radius-lg:24px;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;margin:0;padding:0;}
html,body{
  height:100%;width:100%;
  background-color:#150529;background-image:none;color:var(--text);
  font-family:var(--sans);overflow:hidden;-webkit-font-smoothing:antialiased;
}

.app{
  position:relative;z-index:2;height:100vh;height:100dvh;width:100vw;
  display:grid;grid-template-rows:auto auto auto 1.1fr auto;
  padding:env(safe-area-inset-top,14px) calc(env(safe-area-inset-right,0px) + 24px) calc(env(safe-area-inset-bottom,8px) + 6px) calc(env(safe-area-inset-left,0px) + 24px);
  gap:8px;
  animation:fadeUp .6s cubic-bezier(.16,1,.3,1) both;
}
@keyframes fadeUp{from{opacity:0;transform:translateY(12px) scale(0.98);}to{opacity:1;transform:none;}}

header{
  display:flex;align-items:center;justify-content:space-between;
  background:var(--surface);
  border:1px solid var(--border);border-radius:var(--radius-lg);padding:14px 18px;
  box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 12px 32px rgba(0,0,0,0.3);
}
.brand{display:flex;align-items:center;gap:12px;}
.logo-box-styled {
  width:42px;height:42px;border-radius:12px;
  background:var(--surface-elevated); border:1px solid var(--border-light);
  display:flex;align-items:center;justify-content:center;
  box-shadow:0 0 15px rgba(168,85,247,0.15);
}
.logo-svg {
  width:24px;height:24px;fill:none;stroke:var(--purple);stroke-width:2;stroke-linecap:round;stroke-linejoin:round;
}
.brand h1{font-size:18px;font-weight:700;letter-spacing:-0.02em;color:var(--text);}
.brand p{font-size:11px;color:var(--text3);font-weight:500;margin-top:2px;text-transform:uppercase;letter-spacing:0.04em;}
.hdr-right{display:flex;align-items:center;gap:6px;}
.pill{
  display:flex;align-items:center;gap:5px;padding:6px 11px;
  background:var(--surface-elevated);
  border:1px solid var(--border);border-radius:20px;
  font-size:11px;font-weight:600;color:var(--text3);text-transform:uppercase;letter-spacing:0.04em;
}
.pill .dot{width:6px;height:6px;border-radius:50%;background:var(--text3);}
.pill .dot.live{background:var(--green);box-shadow:0 0 8px rgba(48,209,88,0.75);animation:pulse 1.6s infinite;}
.pill .dot.dead{background:var(--red);box-shadow:0 0 8px rgba(255,69,58,0.65);}
@keyframes pulse{0%,100%{opacity:1;}50%{opacity:.3;}}

.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;}
.stat{
  background:var(--surface);
  border:1px solid var(--border);border-radius:var(--radius);
  padding:12px 6px;text-align:center;
  box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 8px 24px rgba(0,0,0,0.3);
  transition:background 0.25s ease;
}
.stat .lbl{font-size:9.5px;font-weight:600;color:var(--text3);text-transform:uppercase;letter-spacing:0.04em;}
.stat .val{font-size:14px;font-weight:700;color:var(--text);margin-top:1px;font-variant-numeric:tabular-nums;}
.stat .val.go{color:var(--green);animation:blink-val 1s ease infinite;}
.stat .val.warn{color:var(--orange);}
.stat .val.err{color:var(--red);}
@keyframes blink-val{0%,100%{opacity:1;}50%{opacity:.45;}}

main{display:flex;flex-direction:column;align-items:center;justify-content:flex-start;gap:clamp(16px, 4vh, 32px);min-height:0;overflow:hidden;flex:1;}

.log-box{
  width:100%;height:120px;
  background:var(--surface);
  border:1px solid var(--border);border-radius:var(--radius);
  padding:12px 14px;overflow:hidden;
  font-family:var(--mono);font-size:10px;line-height:1.6;color:var(--text2);
  box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 8px 24px rgba(0,0,0,0.3);
}
.log-box .log-hdr{
  display:flex;justify-content:space-between;font-size:8.5px;font-weight:600;color:var(--text3);
  letter-spacing:0.06em;text-transform:uppercase;padding-bottom:4px;margin-bottom:4px;
  border-bottom:1px solid rgba(255,255,255,0.06);
}
.log-body{display:flex;flex-direction:column-reverse;height:calc(100% - 20px);overflow:hidden;}
.log-ln{opacity:0;animation:lnIn .25s cubic-bezier(.16,1,.3,1) forwards;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
@keyframes lnIn{from{opacity:0;transform:translateY(4px);}to{opacity:1;transform:none;}}
.log-ln .ts{color:var(--text3);margin-right:5px;}
.log-ln .c-fwd,.log-ln .c-back{color:var(--blue);font-weight:600;}
.log-ln .c-left,.log-ln .c-right{color:var(--purple);font-weight:600;}
.log-ln .c-stop{color:var(--red);font-weight:600;}
.log-ln .c-led{color:var(--orange);font-weight:600;}

.ctrl-area{position:relative;display:flex;align-items:center;justify-content:center;margin:auto 0;}
.ctrl-ring{
  position:absolute;width:min(54vw,210px);height:min(54vw,210px);border-radius:50%;
  border:2px dashed rgba(168,85,247,0.3);pointer-events:none;
  transition:border-color .3s ease,box-shadow .3s ease;
  box-shadow:inset 0 0 20px rgba(168,85,247,0.05), 0 0 20px rgba(168,85,247,0.05);
}
.ctrl-ring.on{border-color:rgba(168,85,247,0.8);box-shadow:inset 0 0 35px rgba(168,85,247,0.2), 0 0 35px rgba(168,85,247,0.2);animation:rotateRing 16s linear infinite;}
@keyframes rotateRing { to { transform: rotate(360deg); } }

.dpad{
  position:relative;width:min(48vw,185px);height:min(48vw,185px);
  display:grid;grid-template-columns:1fr 1fr 1fr;grid-template-rows:1fr 1fr 1fr;gap:5px;
}
.dpad .dir{
  appearance:none;border:none;outline:none;cursor:pointer;
  background:var(--surface);
  border:1px solid var(--border);
  color:var(--text);display:flex;align-items:center;justify-content:center;
  position:relative;overflow:hidden;
  transition:all .15s cubic-bezier(.16,1,.3,1);
  box-shadow:0 4px 12px rgba(0,0,0,0.3);
}
.dir-up{grid-column:2;grid-row:1;border-radius:16px 16px 8px 8px;}
.dir-left{grid-column:1;grid-row:2;border-radius:16px 8px 8px 16px;}
.dir-right{grid-column:3;grid-row:2;border-radius:8px 16px 16px 8px;}
.dir-down{grid-column:2;grid-row:3;border-radius:8px 8px 16px 16px;}

.dir:active,.dir.active{
  transform:scale(0.92);
  background:rgba(168,85,247,0.2);border-color:rgba(168,85,247,0.5);
  color:var(--purple);
  box-shadow:inset 0 0 15px rgba(168,85,247,0.3), 0 0 15px rgba(168,85,247,0.3);
}
.dir svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;}

.stop-btn{
  grid-column:2;grid-row:2;border-radius:50%;
  appearance:none;border:none;outline:none;cursor:pointer;
  background:var(--blue);
  border:1px solid var(--purple);
  color:#fff;
  font-family:var(--sans);font-size:11px;font-weight:800;letter-spacing:0.04em;
  display:flex;align-items:center;justify-content:center;
  position:relative;overflow:hidden;
  transition:all .15s cubic-bezier(.16,1,.3,1);
  box-shadow:0 0 15px rgba(168,85,247,0.4);
}
.stop-btn::before{
  content:'';position:absolute;inset:-3px;border-radius:50%;
  border:1px solid rgba(168,85,247,0.6);
  animation:stopRipple 2s ease-in-out infinite;
}
@keyframes stopRipple{0%,100%{transform:scale(1);opacity:0.5;}50%{transform:scale(1.15);opacity:0;}}
.stop-btn:active,.stop-btn.active{
  transform:scale(0.9);
  background:var(--purple);border-color:var(--purple);
  box-shadow:0 0 25px rgba(168,85,247,0.8);
}

.led-row{
  display:flex;align-items:center;gap:12px;cursor:pointer;width:100%;
  background:var(--surface);
  border:1px solid var(--border);border-radius:var(--radius);
  padding:12px 16px;transition:all 0.25s cubic-bezier(.16,1,.3,1);
  box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 8px 24px rgba(0,0,0,0.3);
}
.led-row.on{border-color:rgba(168,85,247,0.3);background:var(--surface-elevated);box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 8px 24px rgba(168,85,247,0.15);}
.led-ico{font-size:20px;filter:grayscale(1) brightness(0.4);transition:all 0.25s ease;}
.led-row.on .led-ico{filter:none;transform:scale(1.08);}
.led-info{flex:1;}
.led-info .n{font-size:14px;font-weight:600;}
.led-info .d{font-size:11px;color:var(--text3);margin-top:1px;text-transform:uppercase;letter-spacing:0.04em;}
.ios-toggle{
  width:46px;height:28px;border-radius:14px;position:relative;flex-shrink:0;cursor:pointer;
  background:rgba(255,255,255,0.1);transition:background 0.2s ease;
}
.ios-toggle.on{background:var(--purple);box-shadow:0 0 10px rgba(168,85,247,0.5);}
.ios-toggle .thumb{
  position:absolute;top:2px;left:2px;width:24px;height:24px;border-radius:50%;
  background:#fff;
  box-shadow:0 3px 6px rgba(0,0,0,0.12);
  transition:left 0.2s cubic-bezier(.16,1,.3,1);
}
.ios-toggle.on .thumb{left:20px;}

.acknowledgement{
  font-size:10px;color:var(--text3);font-weight:600;
  text-align:center;padding:8px 16px;letter-spacing:0.04em;text-transform:uppercase;
  background:var(--surface);border:1px solid var(--border);border-radius:18px;
  box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 8px 24px rgba(0,0,0,0.3);
  margin:0 auto;width:fit-content;
  opacity:0.9;animation:pulseText 3s ease-in-out infinite;
  cursor:pointer;
}
.acknowledgement span{color:var(--purple);font-weight:800;text-shadow:0 0 8px rgba(168,85,247,0.5);}
.creator-modal{
  position:fixed;inset:0;z-index:20;
  background:rgba(0,0,0,0.6);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);
  display:flex;align-items:center;justify-content:center;
  opacity:0;visibility:hidden;transition:all 0.3s ease;
}
.creator-modal.show{opacity:1;visibility:visible;}
.creator-card{
  background:var(--surface);border:1px solid var(--border);border-radius:24px;
  width:280px;padding:32px 24px;text-align:center;
  transform:scale(0.9) translateY(10px);transition:all 0.3s cubic-bezier(0.16,1,0.3,1);
  position:relative;box-shadow:inset 0 1px 1px rgba(255,255,255,0.02), 0 12px 40px rgba(0,0,0,0.5);
}
.creator-modal.show .creator-card{transform:scale(1) translateY(0);}
.creator-close{
  position:absolute;top:12px;right:16px;background:none;border:none;
  color:var(--text3);font-size:24px;cursor:pointer;line-height:1;
}
.creator-pic{
  width:80px;height:80px;border-radius:50%;margin:0 auto 16px;
  background:linear-gradient(135deg,var(--blue),var(--purple));padding:2px;
}
.creator-pic img{
  width:100%;height:100%;border-radius:50%;object-fit:cover;
  border:2px solid var(--surface-solid);
}
.creator-name{font-size:18px;font-weight:700;letter-spacing:-0.02em;margin-bottom:4px;}
.creator-title{font-size:11px;color:var(--text2);font-weight:500;text-transform:uppercase;letter-spacing:0.04em;margin-bottom:24px;}
.creator-links{display:flex;align-items:center;justify-content:center;gap:16px;}
.creator-links a{
  display:flex;align-items:center;justify-content:center;
  width:40px;height:40px;border-radius:50%;
  background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);
  color:var(--text);transition:all 0.2s ease;
}
.creator-links a svg{width:18px;height:18px;fill:currentColor;}
.creator-links a:hover,.creator-links a:active{
  background:var(--blue);border-color:var(--blue);transform:scale(1.05);
}
@keyframes pulseText{ 0%, 100% { opacity: 0.7; } 50% { opacity: 1; } }

#boot{
  position:fixed;inset:0;z-index:10;background:var(--bg);
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;
  transition:opacity .5s cubic-bezier(.16,1,.3,1),visibility .5s ease;
}
#boot.hide{opacity:0;visibility:hidden;pointer-events:none;}
.b-logo{
  width:60px;height:60px;margin-bottom:12px;border-radius:16px;
  background:linear-gradient(135deg,var(--blue),var(--purple));
  display:flex;align-items:center;justify-content:center;
  box-shadow:0 0 30px rgba(168,85,247,0.4);
  animation:bBounce 1.8s ease-in-out infinite;
}
@keyframes bBounce{0%,100%{transform:translateY(0) scale(1);}50%{transform:translateY(-5px) scale(1.03);}}
.b-title{font-size:18px;font-weight:800;letter-spacing:0.02em;}
.b-line{font-size:11px;color:var(--text2);margin-top:6px;font-weight:500;}
.b-bar{width:140px;height:3px;border-radius:1.5px;margin-top:10px;background:rgba(255,255,255,0.06);overflow:hidden;}
.b-bar-fill{height:100%;width:0;border-radius:1.5px;background:linear-gradient(90deg,var(--blue),var(--purple));animation:bLoad .9s ease forwards;}
@keyframes bLoad{to{width:100%;}}

@media(orientation:landscape)and(max-height:480px){
  .app{grid-template-rows:auto auto 1fr;grid-template-columns:1fr 1fr;}
  header{grid-column:1/3;}
  .stats{grid-column:1/3;grid-template-columns:repeat(6,1fr);gap:4px;}
  main{grid-column:1/3;flex-direction:row;justify-content:space-evenly;gap:10px;}
  .log-box{max-width:200px;height:100%;}
  .acknowledgement{display:none;}
}
</style>
</head>
<body>

<div id="boot">
  <div class="b-logo">
    <svg class="logo-svg" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
      <path d="M12 7V4M12 4l1.5 1.5" stroke="#ffffff" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>
      <rect x="3" y="7" width="18" height="8" rx="3" stroke="#ffffff" stroke-width="2"/>
      <path d="M8 9.5v3m-1.5-1.5h3" stroke="#ffffff" stroke-width="1.5" stroke-linecap="round"/>
      <circle cx="15.5" cy="10" r="1" fill="#ffffff"/>
      <circle cx="17.5" cy="12" r="1" fill="#ffffff"/>
      <path d="M5 15v2a2 2 0 0 0 4 0v-2" stroke="#ffffff" stroke-width="2"/>
      <path d="M15 15v2a2 2 0 0 0 4 0v-2" stroke="#ffffff" stroke-width="2"/>
    </svg>
  </div>
  <div class="b-title">EspBot</div>
  <div class="b-line" id="bootLine">Waking up...</div>
  <div class="b-bar"><div class="b-bar-fill"></div></div>
</div>

<div class="app">
  <header>
    <div class="brand">
      <div class="logo-box-styled">
        <svg class="logo-svg" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
          <path d="M12 7V4M12 4l1.5 1.5" stroke="#ffffff" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>
          <rect x="3" y="7" width="18" height="8" rx="3" stroke="#ffffff" stroke-width="2"/>
          <path d="M8 9.5v3m-1.5-1.5h3" stroke="#ffffff" stroke-width="1.5" stroke-linecap="round"/>
          <circle cx="15.5" cy="10" r="1" fill="#ffffff"/>
          <circle cx="17.5" cy="12" r="1" fill="#ffffff"/>
          <path d="M5 15v2a2 2 0 0 0 4 0v-2" stroke="#ffffff" stroke-width="2"/>
          <path d="M15 15v2a2 2 0 0 0 4 0v-2" stroke="#ffffff" stroke-width="2"/>
        </svg>
      </div>
      <div>
        <h1>EspBot</h1>
        <p>Wireless Control Unit</p>
      </div>
    </div>
    <div class="hdr-right">
      <div class="pill"><span class="dot" id="connDot"></span><span id="connText">Link</span></div>
    </div>
  </header>

  <div class="stats">
    <div class="stat"><div class="lbl">State</div><div class="val" id="stCommand">Idle</div></div>
    <div class="stat"><div class="lbl">LED</div><div class="val err" id="stLed">Off</div></div>
    <div class="stat"><div class="lbl">Signal</div><div class="val" id="stRssi">--</div></div>
    <div class="stat"><div class="lbl">Ping</div><div class="val" id="stPing">--</div></div>
    <div class="stat"><div class="lbl">Uptime</div><div class="val" id="stUptime">0s</div></div>
    <div class="stat"><div class="lbl">Cmds</div><div class="val" id="stCount">0</div></div>
  </div>

  <div class="led-row" id="ledToggle">
    <div class="led-ico">&#128161;</div>
    <div class="led-info">
      <div class="n">Headlight</div>
      <div class="d" id="ledSub">Tap to turn on</div>
    </div>
    <div class="ios-toggle" id="ledSwitch"><div class="thumb"></div></div>
  </div>

  <main>
    <div class="log-box">
      <div class="log-hdr"><span>Activity Log</span><span id="termClock">00:00:00</span></div>
      <div class="log-body" id="termBody"></div>
    </div>

    <div class="ctrl-area">
      <div class="ctrl-ring" id="dpadRing"></div>
      <div class="dpad" id="dpad">
        <button class="dir dir-up" data-cmd="forward" aria-label="Forward"><svg viewBox="0 0 24 24"><polyline points="6 15 12 9 18 15"/></svg></button>
        <button class="dir dir-left" data-cmd="left" aria-label="Left"><svg viewBox="0 0 24 24"><polyline points="15 18 9 12 15 6"/></svg></button>
        <button class="stop-btn" data-cmd="stop" aria-label="Stop">STOP</button>
        <button class="dir dir-right" data-cmd="right" aria-label="Right"><svg viewBox="0 0 24 24"><polyline points="9 6 15 12 9 18"/></svg></button>
        <button class="dir dir-down" data-cmd="backward" aria-label="Backward"><svg viewBox="0 0 24 24"><polyline points="6 9 12 15 18 9"/></svg></button>
      </div>
    </div>
  </main>

  <div class="acknowledgement" id="ackBtn">Designed by <span>Fayas</span></div>
</div>

<div class="creator-modal" id="creatorModal">
  <div class="creator-card">
    <button class="creator-close" id="creatorClose">&times;</button>
    <div class="creator-pic">
      <img src="https://github.com/MohammadFayasKhan.png" alt="Mohammad Fayas Khan">
    </div>
    <div class="creator-name">Mohammad Fayas Khan</div>
    <div class="creator-title">Developer &amp; Creator</div>
    <div class="creator-links">
      <a href="https://github.com/MohammadFayasKhan" target="_blank" aria-label="GitHub">
        <svg viewBox="0 0 24 24"><path d="M12 2A10 10 0 0 0 2 12c0 4.42 2.87 8.17 6.84 9.5c.5.08.66-.23.66-.5v-1.69c-2.77.6-3.36-1.34-3.36-1.34c-.46-1.16-1.11-1.47-1.11-1.47c-.91-.62.07-.6.07-.6c1 .07 1.53 1.03 1.53 1.03c.87 1.52 2.34 1.07 2.91.83c.09-.65.35-1.09.63-1.34c-2.22-.25-4.55-1.11-4.55-4.92c0-1.11.38-2 1.03-2.71c-.1-.25-.45-1.29.1-2.64c0 0 .84-.27 2.75 1.02c.79-.22 1.65-.33 2.5-.33c.85 0 1.71.11 2.5.33c1.91-1.29 2.75-1.02 2.75-1.02c.55 1.35.2 2.39.1 2.64c.65.71 1.03 1.6 1.03 2.71c0 3.82-2.34 4.66-4.57 4.91c.36.31.69.92.69 1.85V21c0 .27.16.59.67.5C19.14 20.16 22 16.42 22 12A10 10 0 0 0 12 2z"/></svg>
      </a>
      <a href="https://www.linkedin.com/in/mohammadfayaskhan" target="_blank" aria-label="LinkedIn">
        <svg viewBox="0 0 24 24"><path d="M19 0h-14c-2.761 0-5 2.239-5 5v14c0 2.761 2.239 5 5 5h14c2.762 0 5-2.239 5-5v-14c0-2.761-2.238-5-5-5zm-11 19h-3v-11h3v11zm-1.5-12.268c-.966 0-1.75-.79-1.75-1.764s.784-1.764 1.75-1.764 1.75.79 1.75 1.764-.783 1.764-1.75 1.764zm13.5 12.268h-3v-5.604c0-3.368-4-3.113-4 0v5.604h-3v-11h3v1.765c1.396-2.586 7-2.777 7 2.476v6.759z"/></svg>
      </a>
      <a href="https://www.instagram.com/fayaskhanx" target="_blank" aria-label="Instagram">
        <svg viewBox="0 0 24 24"><path d="M12 2.163c3.204 0 3.584.012 4.85.07c3.252.148 4.771 1.691 4.919 4.919c.058 1.265.069 1.645.069 4.849c0 3.205-.012 3.584-.069 4.849c-.149 3.225-1.664 4.771-4.919 4.919c-1.266.058-1.644.07-4.85.07c-3.204 0-3.584-.012-4.849-.07c-3.26-.149-4.771-1.699-4.919-4.92c-.058-1.265-.07-1.644-.07-4.849c0-3.204.013-3.583.07-4.849c.149-3.227 1.664-4.771 4.919-4.919c1.266-.057 1.645-.069 4.849-.069zM12 0C8.741 0 8.333.014 7.053.072C2.695.272.273 2.69.073 7.052C.014 8.333 0 8.741 0 12c0 3.259.014 3.668.072 4.948c.2 4.358 2.618 6.78 6.98 6.98C8.333 23.986 8.741 24 12 24c3.259 0 3.668-.014 4.948-.072c4.354-.2 6.782-2.618 6.979-6.98c.059-1.28.073-1.689.073-4.948c0-3.259-.014-3.667-.072-4.947c-.196-4.354-2.617-6.78-6.979-6.98C15.668.014 15.259 0 12 0zm0 5.838a6.162 6.162 0 1 0 0 12.324a6.162 6.162 0 0 0 0-12.324zM12 16a4 4 0 1 1 0-8a4 4 0 0 1 0 8zm6.406-11.845a1.44 1.44 0 1 0 0 2.881a1.44 1.44 0 0 0 0-2.881z"/></svg>
      </a>
    </div>
  </div>
</div>

<script>
(() => {
  "use strict";

  const $ = id => document.getElementById(id);
  const dpad = $('dpad');
  const dpadRing = $('dpadRing');
  const connDot = $('connDot');
  const connText = $('connText');
  const stCommand = $('stCommand');
  const stLed = $('stLed');
  const stRssi = $('stRssi');
  const stPing = $('stPing');
  const stUptime = $('stUptime');
  const stCount = $('stCount');
  const ledToggle = $('ledToggle');
  const ledSwitch = $('ledSwitch');
  const ledSub = $('ledSub');
  const boot = $('boot');
  const bootLine = $('bootLine');
  const termBody = $('termBody');
  const termClock = $('termClock');
  const ackBtn = $('ackBtn');
  const creatorModal = $('creatorModal');
  const creatorClose = $('creatorClose');

  if(ackBtn && creatorModal) {
    ackBtn.onclick = () => { haptic('light'); creatorModal.classList.add('show'); };
    creatorClose.onclick = () => { haptic('light'); creatorModal.classList.remove('show'); };
    creatorModal.onclick = (e) => { if(e.target === creatorModal) { haptic('light'); creatorModal.classList.remove('show'); } };
  }

  const REQ_TIMEOUT = 1500;
  const HEARTBEAT = 250;
  const POLL_MS = 1000;
  const MAX_LOG = 6;

  let activeCmd = null;
  let hbTimer = null;
  let connected = false;
  let cmdCount = 0;

  function haptic(style) {
    try {
      if (navigator.vibrate) {
        const patterns = {
          light: [8],
          medium: [15],
          heavy: [25],
          error: [15, 40, 15],
          success: [10, 30, 10, 30, 10]
        };
        navigator.vibrate(patterns[style] || patterns.light);
      }
    } catch(e) {}
  }

  const bootMsgs = ['Initializing...','Calibrating motors...','Connecting WiFi...','Ready!'];
  let bmi = 0;
  const bootInt = setInterval(() => {
    bmi++;
    if (bmi < bootMsgs.length) bootLine.textContent = bootMsgs[bmi];
    else clearInterval(bootInt);
  }, 250);

  const t0 = Date.now();
  const tagMap = {forward:'c-fwd',backward:'c-back',left:'c-left',right:'c-right',stop:'c-stop',led:'c-led'};
  function log(text, cls) {
    const el = document.createElement('div');
    el.className = 'log-ln';
    const s = ((Date.now() - t0) / 1000).toFixed(1);
    el.innerHTML = '<span class="ts">+' + s + 's</span><span class="' + (cls||'') + '">' + text + '</span>';
    termBody.insertBefore(el, termBody.firstChild);
    while (termBody.children.length > MAX_LOG) termBody.removeChild(termBody.lastChild);
  }
  log('System online', 'c-led');

  function tickClock() {
    const d = new Date();
    termClock.textContent = [d.getHours(),d.getMinutes(),d.getSeconds()].map(n => String(n).padStart(2,'0')).join(':');
  }
  setInterval(tickClock, 1000); tickClock();

  function request(path) {
    const ac = new AbortController();
    const pt = performance.now();
    const tm = setTimeout(() => ac.abort(), REQ_TIMEOUT);
    return fetch(path, { signal: ac.signal, cache: 'no-store' })
      .then(r => { clearTimeout(tm); markConn(true); stPing.textContent = Math.round(performance.now() - pt) + ' ms'; return r; })
      .catch(e => { clearTimeout(tm); markConn(false); throw e; });
  }
  function markConn(ok) {
    if (ok === connected) return;
    connected = ok;
    connDot.className = 'dot ' + (ok ? 'live' : 'dead');
    connText.textContent = ok ? 'Linked' : 'Offline';
    if (!ok) log('Connection lost', 'c-stop');
  }

  function send(cmd, hb) {
    request('/' + cmd).catch(() => {});
    const lbl = cmd.toUpperCase();
    stCommand.textContent = lbl === 'STOP' ? 'Idle' : lbl;
    const moving = cmd !== 'stop';
    stCommand.className = 'val' + (moving ? ' go' : '');
    dpadRing.classList.toggle('on', moving);
    if (!hb) {
      cmdCount++; stCount.textContent = cmdCount;
      log(moving ? lbl + ' engaged' : 'Stopped', tagMap[cmd]);
      haptic(moving ? 'medium' : 'light');
    }
  }

  function holdStart(cmd) {
    if (activeCmd === cmd) return;
    activeCmd = cmd;
    send(cmd, false);
    clearInterval(hbTimer);
    hbTimer = setInterval(() => { if (activeCmd) send(activeCmd, true); }, HEARTBEAT);
  }
  function holdEnd() {
    if (!activeCmd) return;
    activeCmd = null;
    clearInterval(hbTimer);
    send('stop', false);
  }

  dpad.querySelectorAll('button').forEach(btn => {
    const cmd = btn.dataset.cmd;
    const down = e => {
      e.preventDefault();
      btn.classList.add('active');
      if (cmd === 'stop') {
        send('stop', false);
        activeCmd = null; clearInterval(hbTimer);
        haptic('error');
      } else {
        holdStart(cmd);
      }
    };
    const up = e => {
      e.preventDefault();
      btn.classList.remove('active');
      if (cmd !== 'stop') holdEnd();
    };
    btn.addEventListener('pointerdown', down);
    btn.addEventListener('pointerup', up);
    btn.addEventListener('pointerleave', up);
    btn.addEventListener('pointercancel', up);
    btn.addEventListener('contextmenu', e => e.preventDefault());
  });

  const kmap = {w:'forward',arrowup:'forward',s:'backward',arrowdown:'backward',a:'left',arrowleft:'left',d:'right',arrowright:'right'};
  const k2b = {};
  dpad.querySelectorAll('button').forEach(b => k2b[b.dataset.cmd] = b);
  window.addEventListener('keydown', e => {
    const k = e.key.toLowerCase();
    if (k === ' ') { e.preventDefault(); send('stop', false); activeCmd = null; clearInterval(hbTimer); haptic('error'); return; }
    const cmd = kmap[k]; if (!cmd || e.repeat) return;
    e.preventDefault(); holdStart(cmd);
    k2b[cmd] && k2b[cmd].classList.add('active');
  });
  window.addEventListener('keyup', e => {
    const cmd = kmap[e.key.toLowerCase()]; if (!cmd) return;
    holdEnd(); k2b[cmd] && k2b[cmd].classList.remove('active');
  });

  ledToggle.addEventListener('click', () => {
    const on = !ledSwitch.classList.contains('on');
    request('/led/' + (on ? 'on' : 'off')).catch(() => {});
    setLedUI(on);
    haptic('light');
    log('Headlight ' + (on ? 'ON' : 'OFF'), 'c-led');
  });
  function setLedUI(on) {
    ledSwitch.classList.toggle('on', on);
    ledToggle.classList.toggle('on', on);
    stLed.textContent = on ? 'On' : 'Off';
    stLed.className = 'val' + (on ? ' warn' : ' err');
    ledSub.textContent = on ? 'Tap to turn off' : 'Tap to turn on';
  }

  function emergency() {
    activeCmd = null; clearInterval(hbTimer);
    navigator.sendBeacon ? navigator.sendBeacon('/stop') : fetch('/stop',{keepalive:true}).catch(()=>{});
  }
  window.addEventListener('beforeunload', emergency);
  window.addEventListener('pagehide', emergency);
  document.addEventListener('visibilitychange', () => { if (document.hidden) emergency(); });

  function poll() {
    request('/status').then(r => r.json()).then(d => {
      const lbl = d.command === 'STOPPED' ? 'Idle' : d.command;
      stCommand.textContent = lbl;
      const mv = d.command !== 'STOPPED';
      stCommand.className = 'val' + (mv ? ' go' : '');
      dpadRing.classList.toggle('on', mv);
      stRssi.textContent = d.rssi + ' dBm';
      stUptime.textContent = fmtUp(d.uptime);
      setLedUI(d.led);
    }).catch(() => {});
  }
  function fmtUp(s) {
    if (s < 60) return s + 's';
    const m = Math.floor(s/60), r = s%60;
    if (m < 60) return m + 'm ' + r + 's';
    return Math.floor(m/60) + 'h ' + (m%60) + 'm';
  }
  setInterval(poll, POLL_MS); poll();

  window.addEventListener('load', () => {
    setTimeout(() => { boot.classList.add('hide'); haptic('success'); }, 1100);
  });
})();
</script>
</body>
</html>
)HTML_PAGE";

// ============================================================================
//  SECTION 8: HTTP ROUTES
// ============================================================================

void sendJsonStatus() {
  String json = "{";
  json +=
      "\"command\":\"" + String(commandToString(state.currentCommand)) + "\",";
  json += "\"led\":" + String(state.ledOn ? "true" : "false") + ",";

  int32_t rssi = WiFi.RSSI();
  json += "\"rssi\":" + String(rssi) + ",";

  json += "\"uptime\":" + String(millis() / 1000) + ",";

  String ipStr = WiFi.getMode() == WIFI_STA ? WiFi.localIP().toString()
                                            : WiFi.softAPIP().toString();
  json += "\"ip\":\"" + ipStr + "\",";

  json += "\"clients\":" + String(WiFi.softAPgetStationNum());
  json += "}";

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", json);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", PAGE_INDEX);
}

void handleForward() {
  moveForward();
  touchWatchdog();
  server.send(200, "text/plain", "OK");
}
void handleBackward() {
  moveBackward();
  touchWatchdog();
  server.send(200, "text/plain", "OK");
}
void handleLeft() {
  turnLeft();
  touchWatchdog();
  server.send(200, "text/plain", "OK");
}
void handleRight() {
  turnRight();
  touchWatchdog();
  server.send(200, "text/plain", "OK");
}
void handleStop() {
  stopRobot();
  touchWatchdog();
  server.send(200, "text/plain", "OK");
}

void handleLedOn() {
  setLed(true);
  server.send(200, "text/plain", "OK");
}
void handleLedOff() {
  setLed(false);
  server.send(200, "text/plain", "OK");
}
void handleLedToggle() {
  toggleLed();
  server.send(200, "text/plain", "OK");
}

void handleStatus() { sendJsonStatus(); }

void handleNotFound() {
  server.send(404, "text/plain", "404: Route not found");
}

void registerRoutes() {
  server.on("/", HTTP_GET, handleRoot);

  server.on("/forward", HTTP_GET, handleForward);
  server.on("/backward", HTTP_GET, handleBackward);
  server.on("/left", HTTP_GET, handleLeft);
  server.on("/right", HTTP_GET, handleRight);
  server.on("/stop", HTTP_GET, handleStop);

  server.on("/led/on", HTTP_GET, handleLedOn);
  server.on("/led/off", HTTP_GET, handleLedOff);
  server.on("/led/toggle", HTTP_GET, handleLedToggle);

  server.on("/status", HTTP_GET, handleStatus);

  registerDiagRoutes();

  server.onNotFound(handleNotFound);
}

// ============================================================================
//  SECTION 9: WIFI ACCESS POINT & STATION
// ============================================================================

void startAccessPoint() {
  WiFi.mode(WIFI_AP);

  IPAddress local_IP(192, 168, 9, 1);
  IPAddress gateway(192, 168, 9, 1);
  IPAddress subnet(255, 255, 255, 0);
  bool configResult = WiFi.softAPConfig(local_IP, gateway, subnet);

  WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD, Config::WIFI_CHANNEL, false,
              Config::MAX_CLIENTS);

  IPAddress ip = WiFi.softAPIP();
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.print(F(" Config Result: "));
  Serial.println(configResult ? F("SUCCESS") : F("FAILED"));
  Serial.println(F(" EspBot — ESP8266 WiFi Robot"));
  Serial.println(F("=========================================="));
  Serial.println(F(" ACCESS POINT MODE ACTIVE"));
  Serial.print(F(" SSID:     "));
  Serial.println(Config::AP_SSID);
  Serial.print(F(" Password: "));
  Serial.println(Config::AP_PASSWORD);
  Serial.print(F(" IP:       "));
  Serial.println(ip);
  Serial.println(F(" Access via: http://espbot.local"));
  Serial.println(F("=========================================="));
}

void startWiFi() {
  Serial.print(F("Connecting to home WiFi: "));
  Serial.println(Config::STA_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(Config::STA_SSID, Config::STA_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 16) {
    delay(500);
    Serial.print(F("."));
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("=========================================="));
    Serial.println(F(" CONNECTED TO WIFI HOTSPOT"));
    Serial.print(F(" SSID:     "));
    Serial.println(Config::STA_SSID);
    Serial.print(F(" IP:       "));
    Serial.println(WiFi.localIP());
    Serial.println(F(" Access via: http://espbot.local"));
    Serial.println(F("=========================================="));
  } else {
    Serial.println(F(" Connection failed or timed out."));
    Serial.println(F(" Falling back to Access Point (AP) mode..."));
    startAccessPoint();
  }
}

// ============================================================================
//  SECTION 10: SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(Pins::MOTOR1_IN1, OUTPUT);
  pinMode(Pins::MOTOR1_IN2, OUTPUT);
  pinMode(Pins::MOTOR2_IN3, OUTPUT);
  pinMode(Pins::MOTOR2_IN4, OUTPUT);
  pinMode(Pins::STATUS_LED, OUTPUT);

  stopRobot();
  setLed(false);

  startWiFi();

  WiFi.hostname("espbot");
  if (MDNS.begin("espbot")) {
    Serial.println(F(" mDNS responder started: http://espbot.local"));
  }

  registerRoutes();
  server.begin();

  Serial.println(F(
      " HTTP server started. Connect and open the IP or http://espbot.local"));

  touchWatchdog();
}

// ============================================================================
//  SECTION 11: MAIN LOOP
// ============================================================================

void loop() {
  server.handleClient();
  MDNS.update();
  serviceWatchdog();
}
