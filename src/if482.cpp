#if defined HAS_IF482 && defined RTC_INT

/* NOTE:
The IF482 Generator needs an high precise 1 Hz clock signal which cannot be
acquired in suitable precision on the ESP32 SoC itself. Additional clocking
hardware is required, ususally the clock signal is generated by external RTC or
GPS which can generate a precise time pulse signal (+/- 2ppm).

In this example code we use a Maxim DS3231 RTC chip, and configure the chips's
interrupt output pin as clock. The clock signal triggers an interrupt on the
ESP32, which controls the realtime output of IF482 telegram. This is why code in
IF482.cpp depends on code in RTCTIME.cpp.
*/

///////////////////////////////////////////////////////////////////////////////

/*
IF482 Generator to control clocks with IF482 telegram input (e.g. BÜRK BU190)
   

Example IF482 telegram: "OAL160806F170400"

IF482 Specification:
http://www.mobatime.com/fileadmin/user_upload/downloads/TE-112023.pdf

The IF 482 telegram is a time telegram, which sends the time and date
information as ASCII characters through the serial interface RS 232 or RS 422.

Communication parameters:

Baud rate: 9600 Bit/s
Data bits 7
Parity: even
Stop bit: 1
Jitter: < 50ms

Interface : RS232 or RS422

Synchronization: Telegram ends at the beginning of the second
specified in the telegram

Cycle: 1 second

Format of ASCII telegram string:

Byte  Meaning             ASCII     Hex
 1    Start of telegram   O         4F
 2    Monitoring*         A         41
 3    Time-Season**       W/S/U/L   57 or 53
 4    Year tens           0 .. 9    30 .. 39
 5    Year unit           0 .. 9    30 .. 39
 6    Month tens          0 or 1    30 or 31
 7    Month unit          0 .. 9    30 .. 39
 8    Day tens            0 .. 3    30 .. 33
 9    Day unit            0 .. 9    30 .. 39
10    Day of week***      1 .. 7    31 .. 37
11    Hours tens          0 .. 2    30 .. 32
12    Hours unit          0 .. 9    30 .. 39
13    Minutes tens        0 .. 5    30 .. 35
14    Minutes unit        0 .. 9    30 .. 39
15    Seconds tens        0 .. 5    30 .. 35
16    Seconds unit        0 .. 9    30 .. 39
17    End of telegram     CR        0D

*) Monitoring:
With a correctly received time in the sender unit, the ASCII character 'A' is
issued. If 'M' is issued, this indicates that the sender was unable to receive
any time signal for over 12 hours (time is accepted with ‘A’ and ‘M’).

**) Season:
W: Standard time,
S: Season time,
U: UTC time (not supported by all systems),
L: Local Time

***) Day of week:
not evaluated by model BU-190

*/
///////////////////////////////////////////////////////////////////////////////

#include "if482.h"

// Local logging tag
static const char TAG[] = "main";

TaskHandle_t IF482Task;

HardwareSerial IF482(2); // use UART #2 (note: #1 may be in use for serial GPS)

// initialize and configure IF482 Generator
int if482_init(void) {

  // setup external interupt for active low RTC INT pin
  pinMode(RTC_INT, INPUT_PULLUP);

  // start if482 serial output feed task
  xTaskCreatePinnedToCore(if482_loop,  // task function
                          "if482loop", // name of task
                          2048,        // stack size of task
                          (void *)1,   // parameter of the task
                          3,           // priority of the task
                          &IF482Task,  // task handle
                          0);          // CPU core

  assert(IF482Task); // has if482loop task started?

  // open serial interface
  IF482.begin(HAS_IF482);

  // if we have hardware pps signal we use it as precise time base
#ifdef RTC_INT
// assure we know clock freq
#ifndef RTC_CLK
#error "No RTC clock cycle defined in board hal file"
#endif
  // use external rtc 1Hz clock for triggering IF482 telegram
  if (I2C_MUTEX_LOCK()) {
    Rtc.SetSquareWavePinClockFrequency(DS3231SquareWaveClock_1Hz);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeClock);
    I2C_MUTEX_UNLOCK();
  } else {
    ESP_LOGE(TAG, "I2c bus busy - IF482 initialization error");
    return 0; // failure
  }
  attachInterrupt(digitalPinToInterrupt(RTC_INT), IF482IRQ, FALLING);

// no RTC, thus we use less precise ESP32 hardware timer
#else
  // setup 1000ms clock signal for IF482 generator using esp32 hardware timer 1
  ESP_LOGD(TAG, "Starting IF482 pulse...");
  dcfCycle = timerBegin(1, 8000, true); // set 80 MHz prescaler to 1/10000 sec
  timerAttachInterrupt(dcfCycle, &IF482IRQ, true);
  timerAlarmWrite(dcfCycle, 10000, true); // 1000ms cycle
  timerAlarmEnable(dcfCycle);
#endif

  return 1; // success

} // if482_init

String IF482_Out(time_t tt) {

  time_t t = myTZ.toLocal(tt);
  char mon, buf[14], out[17];

  switch (timeStatus()) { // indicates if time has been set and recently synced
  case timeSet:           // time is set and is synced
    mon = 'A';
    break;
  case timeNeedsSync: // time had been set but sync attempt did not succeed
    mon = 'M';
    break;
  default: // time not set, no valid time
    mon = '?';
    break;
  } // switch

  // do we have confident time/date?
  if ((timeStatus() == timeSet) || (timeStatus() == timeNeedsSync))
    snprintf(buf, sizeof(buf), "%02u%02u%02u%1u%02u%02u%02u", year(t) - 2000,
             month(t), day(t), weekday(t), hour(t), minute(t), second(t));
  else
    snprintf(buf, sizeof(buf), "000000F000000"); // no confident time/date

  // output IF482 telegram
  snprintf(out, sizeof(out), "O%cL%s\r", mon, buf);
  ESP_LOGD(TAG, "IF482 = %s", out);
  return out;
}

void if482_loop(void *pvParameters) {

  configASSERT(((uint32_t)pvParameters) == 1); // FreeRTOS check

  TickType_t wakeTime;
  time_t t, tt;
  const TickType_t timeOffset =
      pdMS_TO_TICKS(IF482_OFFSET); // duration of telegram transmit
  const TickType_t startTime = xTaskGetTickCount(); // now

  // wait until begin of a new second
  t = tt = now();
  do {
    tt = now();
  } while (t == tt);

  BitsPending = true; // start blink in display

  // take timestamp at moment of start of new second
  const TickType_t shotTime = xTaskGetTickCount() - startTime - timeOffset;

  // task remains in blocked state until it is notified by isr
  for (;;) {
    xTaskNotifyWait(
        0x00,           // don't clear any bits on entry
        ULONG_MAX,      // clear all bits on exit
        &wakeTime,      // receives moment of call from isr
        portMAX_DELAY); // wait forever (missing error handling here...)

    // now we're synced to start of second tt and wait
    // until it's time to start transmit telegram for tt+1
    vTaskDelayUntil(&wakeTime, shotTime); // sets waketime to moment of shot
    IF482.print(IF482_Out(now() + 1));
  }
} // if482_loop()

// interrupt service routine triggered by RTC 1Hz precise clock
void IRAM_ATTR IF482IRQ() {
  xTaskNotifyFromISR(IF482Task, xTaskGetTickCountFromISR(), eSetBits, NULL);
  portYIELD_FROM_ISR();
}

#endif // HAS_IF482