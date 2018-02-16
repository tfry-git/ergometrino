/*  Simple Arduino-based ergometer display with differential feedback
 *
 *  See README.md for details and instructions.
 *  
 *
 *  Copyright (c) 2018 Thomas Friedrichsmeier
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
 
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <EEPROM.h>

#define OLED_RESET 4
Adafruit_SSD1306 display (OLED_RESET);

#define INPIN 2                       // Pin that signals clicks / wheel turns. Needs to be a pin that support interrupts
#define DEBOUNCE_MS 60u               // Debounce timeout. Subsequent clicks within this time window will not be counted
#define CM_PER_CLICK 600u             // Centimeters per click (for speed calculation). Actually the display is unit-less. This is ten-thousands of a 1.0 per click.
#define SPEED_ESTIMATE_WINDOW 4000u   // time window to base speed estimates on.
#define SPEED_TIMEOUT_WINDOW 4000u    // duration since last click, when to assume bike has stopped
#define SEGMENT_LENGTH 500u           // segment length in meters
#define SEGMENT_COUNT 200u            // max number of segments NOTE: cannot go above 256, without adjusting indexing type, too

#define RESET_BEST_PIN 9              // Pull this pin low during power-up to reset recorded best time
#define NOUPDATE_PREV_PIN 8           // Pull this pin low to prevent updates to the previous run recording
#define NOUPDATE_BEST_PIN 7           // Pull this pin low to prevent updates to the recorded best time
#define NEAR_BEST_LED_PIN 6           // An LED connected to this pin will light up, while within 1% or above the current best speed
#define TOO_SLOW_LED_PIN 5            // An LED connected to this pin will light up, while more than 10% below current best speed

#define microtime_t uint32_t  // Used for handling small time (differences). In the present implementaiton, these are quite simply milliseconds
#define macrotime_t uint16_t  // Used for handling larger times in a memory efficient way. These are quarter-seconds

#define EEPROM_OFFSET 64u     // Reserve some room for magic number and future extensions.
#define EEPROM_ADDR_OF_SEGMENT(segment) (EEPROM_OFFSET + (segment)*sizeof(macrotime_t))
const uint16_t EEPROM_MAGIC_NUMBER=0xe1e0;   // If first two bytes of EEPROM match this arbitrary value, assume it initialized

volatile uint32_t last_click = 0;
volatile uint8_t unhandled_click_count;
uint32_t time_base;
uint32_t stopped_since;
uint16_t total_click_count;
macrotime_t macronow;

struct StoredSegment {
  macrotime_t start_time = 0;
  uint16_t spd;
  macrotime_t finish_time = 0;
} prev_run_this_segment, best_run_this_segment;

// A circular buffer to store the last few clicks, so we can provide a smoothed approximation of speed
#define CLICK_BUF_SIZE 10u
microtime_t click_buf[CLICK_BUF_SIZE];
int8_t click_buf_pos = 0;  // keep this signed!

// A circular buffer of speed readings from the past few seconds, for a fancy speed graph
#define SPEED_GRAPH_WIDTH 64u
uint16_t speed_graph[SPEED_GRAPH_WIDTH] = {0};

const uint32_t spd_mult = CM_PER_CLICK * 360ul;

macrotime_t segment_times[SEGMENT_COUNT];
uint8_t current_segment;

#define CLICKS_PER_SEGMENT ((100u * SEGMENT_LENGTH) / CM_PER_CLICK)
inline uint8_t currentSegment () {
  return total_click_count / CLICKS_PER_SEGMENT;
}

/** Returns a time representation suitable for storing long times in a 16 bit value. Correcsponds to quarter-seconds. Timing Is stopped while bike is stopped. */
macrotime_t getMacroTime () {
  uint32_t now;
  if (stopped_since) {
    now = stopped_since - time_base;
  } else {
    now = millis () - time_base;
  }
  return now / 250;
}

/** Simply a type-safe and slightly more readable wrapper around millis (). */
inline microtime_t getMicroTime () {
  return millis ();
}

inline uint32_t macroTimeToMillis (macrotime_t time) {
  return time * 250ul;
}

void handleClick () {
  uint32_t now = millis ();
  if (!last_click || (now - last_click > DEBOUNCE_MS)) {
    last_click = millis ();
    ++unhandled_click_count;
  }
}

void setup()   {
  // NOTE: Serial disabled by default, as it takes up quite a bit of RAM (for the buffers), and we're already tight for RAM
//  Serial.begin(9600);

  pinMode (INPIN, INPUT_PULLUP);
  attachInterrupt (digitalPinToInterrupt (INPIN), handleClick, FALLING);
  pinMode (RESET_BEST_PIN, INPUT_PULLUP);
  pinMode (NOUPDATE_PREV_PIN, INPUT_PULLUP);
  pinMode (NOUPDATE_BEST_PIN, INPUT_PULLUP);
  pinMode (NEAR_BEST_LED_PIN, OUTPUT);
  pinMode (TOO_SLOW_LED_PIN, OUTPUT);
  
  display.begin (SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay ();
  display.display ();

  time_base = millis ();
  stopped_since = time_base;
  for (byte i = 0; i < CLICK_BUF_SIZE; ++i) {
    click_buf[i] = 0;
  }
  unhandled_click_count = 0;
  total_click_count = 0;

  uint16_t dummy;
  EEPROM.get (0, dummy);
  if (dummy != EEPROM_MAGIC_NUMBER) {
    for (uint16_t i = 0; i < E2END; ++i) {
      EEPROM.write (i, 0);
    }
    EEPROM.put (0, EEPROM_MAGIC_NUMBER);
  } else if (!digitalRead (RESET_BEST_PIN)) {
    for (uint16_t i = 0; i < SEGMENT_COUNT; ++i) {
      const macrotime_t dummy = 0;
      EEPROM.put (EEPROM_ADDR_OF_SEGMENT (SEGMENT_COUNT + i), dummy);
    }
  }

  handleNextSegment (0);
}

inline uint16_t getSpeedMacro (uint16_t clicks, macrotime_t time) {
  return (getSpeed (clicks, macroTimeToMillis (time)));
}

uint16_t getSpeed (uint16_t clicks, uint32_t ms) {
  if (clicks < 1) return 0;
  return (spd_mult * clicks) / ms;
}

/** Get the current speed estimate from the lastest click readings. Returned as km/h * 10, so as to have a decimal fraction. */
uint16_t getCurrentSpeed () {
  int8_t clicks = 0;

  microtime_t last = click_buf[click_buf_pos];
  if (getMicroTime () - last > SPEED_TIMEOUT_WINDOW) return 0;

  int8_t pos = click_buf_pos;
  microtime_t elapsed = 0;
  while (clicks < (CLICK_BUF_SIZE - 1)) {  // combine readings from - roughly - as many ms as the speed estimate window. At high RPM this gives better estimates than simply lastest click interval
    if (clicks >= total_click_count) break;  // special casing for start of run, when click buffer is not full, yet
    if (--pos < 0) pos = CLICK_BUF_SIZE - 1;
    microtime_t val = last - click_buf[pos];
    if (val > SPEED_ESTIMATE_WINDOW) break;
    elapsed = val;
    ++clicks;
  }
  if (!clicks) return 0;

/*  Serial.print (clicks);
  Serial.print (" / ");
  Serial.print (elapsed);
  Serial.print (" : ");
  Serial.println (getSpeed (clicks, elapsed)); */

  return getSpeed (clicks, elapsed);
}

// Indicate low battery state -- Only for AVR 3.3. V!
void displayBatteryLow (uint8_t x, uint8_t y) {
#if defined (__AVR__)
#if F_CPU > 8000000UL
#warning You are probably using a 5V processor. This detection algo only makes sense for 3.3V, directly powered by LiPo. Not a problem, but you are not going to get meaningful batter level detection.
#endif
/// For supply voltage measurement. Taken from https://jeelabs.org/2012/05/04/measuring-vcc-via-the-bandgap/
  analogRead(6);    // set up "almost" the proper ADC readout
  bitSet(ADMUX, 3); // then fix it to switch to channel 14
  delayMicroseconds(250); // delay substantially improves accuracy
  bitSet(ADCSRA, ADSC);
  while (bit_is_set(ADCSRA, ADSC))
    ;
  word vread = ADC;
  bool low_battery = vread && (vread >= 330u);   // Corresponds to a warning at nominally ~3.4V (1023*1.1V/3.4V). Note that the AVR can be expected to be stable down to at least 3V, so still leaves a good bit of room.
                                                 // Also note that the reading will not be terribly accurate, so don't expect to see it at _exactly_ 3.4V.
/// end

#else
  bool low_battery = false;
#warning Low battery detection not implemented for this board. Please implement it yourself!
#endif
  display.setCursor (x, y);
  if (low_battery) display.print ("LOW BAT");
}

// Display a number right aligned, divided by ten (with one decimal place)
void displayFractional (uint16_t num) {
  if (num < 100) display.print (" ");
  display.print (num / 10);
  display.print (".");
  display.print (num % 10);
}

// Display a number right aligned, divided by 100 (with two decimal places)
void displayFractionalB (uint16_t num) {
  if (num < 1000) display.print (" ");
  display.print (num / 100);
  display.print (".");
  display.print ((num % 100) / 10);
  display.print ((num % 100) % 10);
}

// Display a (macro) time
void displayTime (macrotime_t time, bool show_hours) {
  if (show_hours) {
    uint8_t hours = time / 14400;
    display.print (hours);
    display.print (":");
  }

  uint8_t minutes = (time / 240);
  if (show_hours) minutes = minutes % 60;
  if (minutes < 10) display.print ("0");
  display.print (minutes);
  display.print (":");

  uint8_t seconds = (time / 4) % 60;
  if (seconds < 10) display.print ("0");
  display.print (seconds);
}

// Display a (macro) time difference
void displayDifferentialTime (macrotime_t now, macrotime_t compare) {
  if (!compare) {
    display.print ("  --:--");
    return;
  }

  macrotime_t diff;
  if (now > compare) {
    display.print ("+ ");
    diff = now - compare;
  } else {
    if (now == compare) display.print ("  ");
    else display.print ("- ");
    diff = compare - now;
  }
  displayTime (diff, false);
}

void displaySpeedGraph (uint8_t x, uint8_t y, uint8_t height, uint8_t offset) {
  // Use a range +/- 7 km/h around current speed. Previously, I tried dynamic scaling based on the range of recorded speeds,
  // but this makes the display hard to interpret
  int16_t center = speed_graph[offset];
  int16_t min_v = max (0, center - 70);
  int16_t max_v = center + 70;
  uint16_t rng = max_v - min_v;

  // draw the actual graph
  for (uint8_t i = 0; i < SPEED_GRAPH_WIDTH; ++i) {
    uint16_t cur = speed_graph[offset+1];
    display.drawPixel (x + i, y + ((max_v - cur) * (uint32_t) height) / rng, WHITE);
    if (++offset >= SPEED_GRAPH_WIDTH) offset = 0;
  }

  // draw reference lines for previous and best (average) speed in the current segment (if in display range)
  for (int i = 0; i < 2; ++i) {
    uint16_t prev = i ? best_run_this_segment.spd : prev_run_this_segment.spd;
    if (prev >= min_v && prev <= max_v) {
      uint8_t prevline_y = y + ((max_v - prev) * (uint32_t) height) / rng;
      for (uint8_t i = 0; i < SPEED_GRAPH_WIDTH; i += 2) {
        display.drawPixel (x + i, prevline_y, WHITE);
      }  
    }
  }
}

// Retrieve the given segment's timing data from EEPROM. Note: Segments above SEGMENT_COUNT are the "best run", segments below SEGMENT_COUNT are the "previous run"
void readSegmentInfo (uint8_t num, StoredSegment* segment) {
  segment->start_time = segment->finish_time;
  EEPROM.get (EEPROM_ADDR_OF_SEGMENT (num), segment->finish_time);
  segment->spd = getSpeedMacro (CLICKS_PER_SEGMENT, segment->finish_time - segment->start_time);
}

// Switch to next segment of the training
void handleNextSegment (uint8_t segment) {
  if (segment) {
    if (digitalRead (NOUPDATE_PREV_PIN)) EEPROM.put (EEPROM_ADDR_OF_SEGMENT (current_segment), macronow);
    macrotime_t prevbest = best_run_this_segment.finish_time;
    if ((prevbest == 0) || (macronow < prevbest)) {
      if (digitalRead (NOUPDATE_BEST_PIN)) EEPROM.put (EEPROM_ADDR_OF_SEGMENT (SEGMENT_COUNT + current_segment), macronow);   
    }
  }

  current_segment = segment;
  readSegmentInfo (segment, &prev_run_this_segment);
  readSegmentInfo (segment + SEGMENT_COUNT, &best_run_this_segment);
}

// Interpolate time at the current position between the given stored segment's start and finish times.
macrotime_t getCurrentPar (const StoredSegment &compare) {
  if (!compare.finish_time) return 0;
  uint16_t clicks_in_segment = total_click_count % CLICKS_PER_SEGMENT;
/*  Serial.print (compare.finish_time);
  Serial.print (" ");
  Serial.print (compare.start_time);
  Serial.print (" ");
  Serial.print (clicks_in_segment);
  Serial.print (" ");
  Serial.println (compare.start_time + ((compare.finish_time - compare.start_time) * clicks_in_segment) / CLICKS_PER_SEGMENT); */
  return (compare.start_time + ((compare.finish_time - compare.start_time) * clicks_in_segment) / CLICKS_PER_SEGMENT);
}

void loop () {
  noInterrupts ();
  while (unhandled_click_count) {
    ++click_buf_pos;
    if (click_buf_pos >= CLICK_BUF_SIZE) click_buf_pos = 0;
    click_buf[click_buf_pos] = last_click;
    --unhandled_click_count;
    ++total_click_count;
  }
  interrupts ();

  // NOTE: All the code below is rather slow, weighing it at above 100ms (on an 8MHz CPU; 70ms of that for the display.display() line). That's not a problem in my use case, as
  //       click readings are handled in an ISR, but if you want to handle much higher RPM, you may have to optimize a bit, or smarten up the ISR.

  uint16_t spd = getCurrentSpeed ();
  // Stop the stopwatch, when bike is stopped
  if (!spd && !stopped_since) {
    stopped_since = millis ();
    if (stopped_since == 0) stopped_since = 1;
  } else if (stopped_since && spd) {
    time_base += millis () - stopped_since;
    stopped_since = 0;
  }
  macronow = getMacroTime ();

  display.clearDisplay ();

  // show current speed
  display.setTextSize (1);
  display.setFont (&FreeMonoBold9pt7b);
  display.setTextColor (WHITE);
  display.setCursor (0,11);
  displayFractional (spd);

  // show total distance
  display.setCursor (73, 11);
  displayFractionalB ((total_click_count * (uint32_t) CM_PER_CLICK) / 1000);

  // show average speed
  display.setFont ();
  display.setCursor (18,18);
  if (total_click_count > 5) displayFractional (getSpeedMacro (total_click_count, getMacroTime ()));

  // show current time
  display.setCursor (85,18);
  displayTime (macronow, true);

  // differential time
  if (current_segment != currentSegment ()) {
    handleNextSegment (currentSegment ());
  }
  display.setCursor (85,27);
  displayDifferentialTime (macronow, getCurrentPar (prev_run_this_segment));
  display.setCursor (85,36);
  displayDifferentialTime (macronow, getCurrentPar (best_run_this_segment));

  // quick status LEDs
  digitalWrite (TOO_SLOW_LED_PIN, (((uint32_t) spd * 10) / 9 < best_run_this_segment.spd));
  digitalWrite (NEAR_BEST_LED_PIN, (((uint32_t) spd * 100) / 99 > best_run_this_segment.spd));

  // speed graph
  uint8_t second = (macronow / 4) % SPEED_GRAPH_WIDTH;
  if (spd) speed_graph[second] = spd;
  displaySpeedGraph (0, 31, 32, second);

  // battery status indication
  displayBatteryLow (85, 57);

/* // For performance measurement
  static uint32_t base;
  display.setCursor (85, 44);
  display.print (millis () - base);
  base = millis (); */

  display.display ();
}

