# RC Car Pin Map

## Current In-Use Pins

- `GPIO1`: battery sense
- `GPIO2`: motor PWM
- `GPIO4`: steering servo PWM
- `GPIO5`: motor direction 1
- `GPIO6`: motor direction 2
- `GPIO41`: microphone clock
- `GPIO42`: microphone data
- `GPIO43`: status LED

### Camera Pins

- `GPIO10`: XCLK
- `GPIO11`: D6
- `GPIO12`: D5
- `GPIO13`: PCLK
- `GPIO14`: D4
- `GPIO15`: D0
- `GPIO16`: D3
- `GPIO17`: D1
- `GPIO18`: D2
- `GPIO38`: VSYNC
- `GPIO39`: SIOC
- `GPIO40`: SIOD
- `GPIO47`: HREF
- `GPIO48`: D7

## Free Pin Candidates

- `GPIO3`
- `GPIO7`
- `GPIO8`
- `GPIO9`
- `GPIO44`

`GPIO44` is intentionally left as a lower-priority candidate to preserve debug and serial flexibility.

## Recommended Expansion Map

### TB6612FNG

- `PWM`: `GPIO2`
- `AIN1 / DIR1`: `GPIO5`
- `AIN2 / DIR2`: `GPIO6`
- `STBY`: `GPIO9`

### Steering Servo

- `PWM`: `GPIO4`

### Status LED

- `LED`: `GPIO43`

### MAX98357A I2S Amplifier

- `BCLK`: `GPIO7`
- `LRC / WS`: `GPIO8`
- `DIN`: `GPIO3`

## Summary

The current board and firmware layout can support:

- camera
- microphone
- DC motor output
- steering servo
- status LED
- one MAX98357A-class I2S amp module

This conclusion is about board capability and pin availability only. Speaker playback still requires firmware implementation.
