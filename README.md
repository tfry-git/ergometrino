# ergorino
Simple arduino-based ergometer monitor with differential feedback

# Problem statement
Cardio-workout is boring, particularly when doing it indoors. Several existing projects try to alleviate this by doing cool stuff such as
coupling the ergometer to a game console, or even simulating a real bicycle ride in VR. Cool as these are, technically, they don't really help much:
Workout is still boring. So, instead, I'd like to be able to just read a book or watch TV while training. But then it's difficult to keep up a
steady pace.

The idea, here, is to focus on the latter problem, and provide straight-forward feedback, on whether your current level of training is good enough,
or you should try to speed up. However, the "good enough" level will vary not only per person, but also over time (long, term, as you get better,
but also within a training session, as for instance, it's near impossible to go at full speed before you've warmed up). Therefore, the idea is simply
to record a) the previous run and b) the best run (aka highscore), and then provide feedback on how you are currently faring compared to those runs.

# Hardware
The hardware requirements are deliberately minimal.

## Microprocessor
As the basis, you'll need at least an ATMEGA328 based arduino (or something better), but it does not matter whether it's a Uno, Nano, whatever. Personally
I picked an Arduino Pro Mini at 3.3V 8MHz. Pretty much anything supported by the arduino enviroment should work, however (including STM), as long as your
display has a driver.

## Ergometer connection
Your ergometer will probably have some sort of reed switch or Hall switch sensor for sensing revolutions of the fly wheel or the pedals. Chances are your
can plug right into that. Connect between Gnd and Pin D2 (by default) of your Arduino. If you cannot find an existing interface, just attach a magnet to the pedals, and a
reed switch at a point where the magnet will pass.

The top of the sketch has some defines that you can tweak to convert this into a credible km/h or mph reading.

## Display
This sketch assumes an SSD1306 based 128*64 monochrome display. These are one of the cheapest and easiest display options available, today. Just connect to VCC and Gnd,
SCL, and SCA (Arduino: A4, and A5), and voilá, you're done. However, any other display with similar dimensions and an Adafruit_GFX-compatible driver will work just as
well. You may have to tweak some details, if your display geometry is smaller, but that should not be much of a problem.

## Optional jumpers and LEDs
_Optionally_, you can connect LEDs (with appropriate resistors) to D6 and D5, for indication of "you're doing well, you're near or above your best level", and "you're doing
poorly, you're significantly below your best level", respectively. Note: These LEDs indicate the relation between your current speed, and the best recorded speed
_in the current phase of the training_.

Connect the following pins to Gnd to achieve specific non-standard behavior:
- D9: Reset best run recording
- D8: Do not update previous run recording
- D7: do not update best run recording

## Storage
To keep things simple, all data is stored in the internal EEPROM. No need to connect any external storage. (However, for this reason, there is also no room to store much
more elaborate data).

# Feature details

## Numeric displays
- Left hand: Current speed (large) - Average speed - Graph of the speed over the past minute (roughly)
- Right hand: Distance - Time - Time difference to previous run - Time difference to best run

## Speed graph
TODO: explain me

## Future extensions

# Status
Intial version basically functional

