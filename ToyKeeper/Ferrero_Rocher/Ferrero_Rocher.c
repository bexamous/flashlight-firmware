/*
 * NANJG 105C Diagram
 *           ---
 *         -|   |- VCC
 *  Star 4 -|   |- Voltage ADC
 *  Star 3 -|   |- PWM
 *     GND -|   |- Star 2
 *           ---
 */

#define F_CPU 4800000UL

// PWM Mode
#define PHASE 0b00000001
#define FAST  0b00000011

/*
 * =========================================================================
 * Settings to modify per driver
 */

#define LOW_TO_HIGH 0
#define VOLTAGE_MON			// Comment out to disable - ramp down and eventual shutoff when battery is low
#define MODES			0,1,3,12,40,125,255		// Must be low to high, and must start with 0
//#define ALT_MODES		0,1,3,12,40,125,255		// Must be low to high, and must start with 0, the defines the level for the secondary output. Comment out if no secondary output
#define MODE_PWM		0,PHASE,PHASE,FAST,FAST,FAST,PHASE		// Define one per mode above, 0 for phase-correct, 1 for fast-PWM
// (use PHASE for max to avoid a lingering moon-like mode while holding the button, side effect of FAST mode)
#define VOLTAGE_IN_PROGRESS_LVL 1  // what level to use while reading voltage before battery check readout
#define TURBO				// Comment out to disable - full output with a step down after n number of seconds
							// If turbo is enabled, it will be where 255 is listed in the modes above
#define TURBO_TIMEOUT	5625 // How many WTD ticks before before dropping down (.016 sec each)
							// 90  = 5625
							// 120 = 7500
							
#define ADC_42			185	// the ADC value we expect for 4.20 volts
#define VOLTAGE_FULL		169 // 3.9 V, 4 blinks
#define VOLTAGE_GREEN	154 // 3.6 V, 3 blinks
#define VOLTAGE_YELLOW	139 // 3.3 V, 2 blinks
#define VOLTAGE_RED		124 // 3.0 V, 1 blink
#define ADC_LOW			123 // When do we start ramping down
#define ADC_CRIT		113 // When do we shut the light off
#define ADC_DELAY		188	// Delay in ticks between low-bat rampdowns (188 ~= 3s)
#define OWN_DELAY
#define BLINK_ON_POWER

/*
 * =========================================================================
 */

#ifdef OWN_DELAY
#include <util/delay_basic.h>
// Having own _delay_ms() saves some bytes AND adds possibility to use variables as input
static void _delay_ms(uint16_t n)
{
	while(n-- > 0)
		_delay_loop_2(950);
}
#else
#include <util/delay.h>
#endif

#include <avr/pgmspace.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
//#include <avr/power.h>

//#define STAR3_PIN   PB4     // If not connected, will cycle L-H.  Connected, H-L
#define SWITCH_PIN  PB3		// what pin the switch is connected to, which is Star 4
#define PWM_PIN     PB1
//#define ALT_PWM_PIN PB0
#define VOLTAGE_PIN PB2
#define RED_PIN     PB4		// pin 3
#define GREEN_PIN   PB0		// pin 5
#define ADC_CHANNEL 0x01	// MUX 01 corresponds with PB2
#define ADC_DIDR 	ADC1D	// Digital input disable bit corresponding with PB2
#define ADC_PRSCL   0x06	// clk/64

#define PWM_LVL     OCR0B   // OCR0B is the output compare register for PB1
#define ALT_PWM_LVL OCR0A   // OCR0A is the output compare register for PB0

#define DB_REL_DUR 0b00001111 // time before we consider the switch released after
							  // each bit of 1 from the right equals 16ms, so 0x0f = 64ms

// Switch handling
#define LONG_PRESS_DUR   21 // How many WDT ticks until we consider a press a long press
                            // 32 is roughly .5 s	

/*
 * The actual program
 * =========================================================================
 */

/*
 * global variables
 */
const uint8_t modes[]     = { MODES    };
#ifdef ALT_MODES
const uint8_t alt_modes[] = { ALT_MODES };
#endif
const uint8_t mode_pwm[] = { MODE_PWM };
volatile uint8_t mode_idx = 0;
volatile uint8_t press_duration = 0;
volatile uint8_t voltage_readout = 0;
uint8_t voltages[] = { VOLTAGE_FULL, VOLTAGE_FULL, VOLTAGE_FULL, VOLTAGE_FULL };

// Debounced switch press value
int is_pressed()
{
	// Keep track of last switch values polled
	static uint8_t buffer = 0x00;
	// Shift over and tack on the latest value, 0 being low for pressed, 1 for pulled-up for released
	buffer = (buffer << 1) | ((PINB & (1 << SWITCH_PIN)) == 0);
	return (buffer & DB_REL_DUR);
}

inline void next_mode() {
	if (++mode_idx >= sizeof(modes)) {
		// Wrap around
		mode_idx = 0;
	}	
}

inline void prev_mode() {
	if (mode_idx == 0) {
		// Wrap around
		mode_idx = sizeof(modes) - 1;
	} else {
		--mode_idx;
	}
}

inline void PCINT_on() {
	// Enable pin change interrupts
	GIMSK |= (1 << PCIE);
}

inline void PCINT_off() {
	// Disable pin change interrupts
	GIMSK &= ~(1 << PCIE);
}

// Need an interrupt for when pin change is enabled to ONLY wake us from sleep.
// All logic of what to do when we wake up will be handled in the main loop.
EMPTY_INTERRUPT(PCINT0_vect);

inline void WDT_on() {
	// Setup watchdog timer to only interrupt, not reset, every 16ms.
	cli();							// Disable interrupts
	wdt_reset();					// Reset the WDT
	WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
	WDTCR = (1<<WDTIE);				// Enable interrupt every 16ms
	sei();							// Enable interrupts
}

inline void WDT_off()
{
	cli();							// Disable interrupts
	wdt_reset();					// Reset the WDT
	MCUSR &= ~(1<<WDRF);			// Clear Watchdog reset flag
	WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
	WDTCR = 0x00;					// Disable WDT
	sei();							// Enable interrupts
}

inline void ADC_on() {
	ADMUX  = (1 << REFS0) | (1 << ADLAR) | ADC_CHANNEL; // 1.1v reference, left-adjust, ADC1/PB2
    DIDR0 |= (1 << ADC_DIDR);							// disable digital input on ADC pin to reduce power consumption
	ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;   // enable, start, prescale
}

inline void ADC_off() {
	ADCSRA &= ~(1<<7); //ADC off
}

void sleep_until_switch_press()
{
	// This routine takes up a lot of program memory :(
	// Turn the WDT off so it doesn't wake us from sleep
	// Will also ensure interrupts are on or we will never wake up
	WDT_off();
	// Need to reset press duration since a button release wasn't recorded
	press_duration = 0;
	// Enable a pin change interrupt to wake us up
	// However, we have to make sure the switch is released otherwise we will wake when the user releases the switch
	while (is_pressed()) {
		_delay_ms(16);
	}
	PCINT_on();
	// Enable sleep mode set to Power Down that will be triggered by the sleep_mode() command.
	//set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	// Now go to sleep
	sleep_mode();
	// Hey, someone must have pressed the switch!!
	// Disable pin change interrupt because it's only used to wake us up
	PCINT_off();
	// Turn the WDT back on to check for switch presses
	WDT_on();
	// Go back to main program
}

// The watchdog timer is called every 16ms
ISR(WDT_vect) {

#ifdef TURBO
	static uint16_t turbo_ticks = 0;
#endif
	static uint8_t  ontime_ticks = 0;
	static uint8_t  lowbatt_cnt = 0;
	uint16_t        voltage = 0;
	uint8_t         i = 0;

	if (mode_idx == 0) {
		ontime_ticks = 0;
	} else {
		if (ontime_ticks < 255) {
			ontime_ticks ++;
		}
	}

	if (is_pressed()) {
		if (press_duration < 255) {
			press_duration++;
		}

		if ((press_duration%LONG_PRESS_DUR) == (LONG_PRESS_DUR-1)) {
			// Long press
#if LOW_TO_HIGH
			prev_mode();
#else
			next_mode();
#endif
		}
		// let the user keep holding the button to keep cycling through modes
		if (press_duration == LONG_PRESS_DUR*2) {
			press_duration = LONG_PRESS_DUR;
		}
		// Just always reset turbo timer whenever the button is pressed
		turbo_ticks = 0;
		// Same with the ramp down delay
		lowbatt_cnt = 0;
	} else {
		// Not pressed
		if (press_duration > 0 && press_duration < LONG_PRESS_DUR) {
			// Short press
#if LOW_TO_HIGH
			next_mode();
#else
			prev_mode();
#endif
			// If the user keeps short-tapping the button from off, reset the
			// on-time timer...  otherwise, if we've been on for a while, ignore
			if (ontime_ticks < (LONG_PRESS_DUR*2)) {
				ontime_ticks = 1;
				// If the user short-tapped all the way through the modes and went
				// to "off" again, start the voltage readout mode
				if (mode_idx == 0) {
					voltage_readout = 1;
				}
			}
		} else {
			// Only do turbo check when switch isn't pressed
#ifdef TURBO
			if (modes[mode_idx] == 255) {
				turbo_ticks++;
				if (turbo_ticks > TURBO_TIMEOUT) {
					// Go to the previous mode
					prev_mode();
				}
			}
#endif
			// Only do voltage monitoring when the switch isn't pressed
#ifdef VOLTAGE_MON
			// See if conversion is done
			if (ADCSRA & (1 << ADIF)) {
				// Get an average of the past few readings
				for (i=0;i<3;i++) {
					voltages[i] = voltages[i+1];
				}
				voltages[3] = ADCH;
				voltage = (voltages[0]+voltages[1]+voltages[2]+voltages[3]) >> 2;
				// See if voltage is lower than what we were looking for
				if (voltage < ((mode_idx == 1) ? ADC_CRIT : ADC_LOW)) {
					++lowbatt_cnt;
				} else {
					lowbatt_cnt = 0;
				}
				if (voltage > VOLTAGE_GREEN) {
					// turn on green LED
					DDRB = (1 << PWM_PIN) | (1 << GREEN_PIN);
					PORTB |= (1 << GREEN_PIN);
					PORTB &= 0xff ^ (1 << RED_PIN);
				}
				else if (voltage > VOLTAGE_YELLOW) {
					// turn on red+green LED (yellow)
					DDRB = (1 << PWM_PIN) | (1 << GREEN_PIN) | (1 << RED_PIN);
					PORTB |= (1 << GREEN_PIN) | (1 << RED_PIN);
				}
				else {
					// turn on red LED
					DDRB = (1 << PWM_PIN) | (1 << RED_PIN);
					PORTB |= (1 << RED_PIN);
					PORTB &= 0xff ^ (1 << GREEN_PIN);
				}
				if (voltage_readout) {
					char blinks = 0;
					// division takes too much flash space
					//voltage = (voltage-ADC_LOW) / (((ADC_42 - 15) - ADC_LOW) >> 2);
					if (voltage >= ADC_42) {
						blinks = 5;
					}
					else if (voltage > VOLTAGE_FULL) {
						blinks = 4;
					}
					else if (voltage > VOLTAGE_GREEN) {
						blinks = 3;
					}
					else if (voltage > VOLTAGE_YELLOW) {
						blinks = 2;
					}
					else if (voltage > ADC_LOW) {
						blinks = 1;
					}
					PWM_LVL = 0;
					_delay_ms(1000);
					// blink up to four times to show voltage
					// (~0%, ~25%, ~50%, ~75%, ~100%)
					for(i=0; i<blinks; i++) {
						PWM_LVL = 12;
						_delay_ms(100);
						PWM_LVL = 0;
						_delay_ms(400);
					}
					voltage_readout = 0;
				}
			}

			// See if it's been low for a while
			if (lowbatt_cnt >= ADC_DELAY) {
				prev_mode();
				lowbatt_cnt = 0;
			}

			// Make sure conversion is running for next time through
			ADCSRA |= (1 << ADSC);
		#endif
		}
		press_duration = 0;
	}
}

int main(void)
{
	// Set all ports to input, and turn pull-up resistors on for the inputs we are using
	DDRB = 0x00;
	PORTB = (1 << SWITCH_PIN);
	// PORTB = (1 << SWITCH_PIN) | (1 << STAR3_PIN);

	// Set the switch as an interrupt for when we turn pin change interrupts on
	PCMSK = (1 << SWITCH_PIN);

	// Set PWM pin to output, and also red/green battery indicator pins
	#ifdef ALT_MODES
	DDRB = (1 << PWM_PIN) | (1 << ALT_PWM_PIN);
	#else
	DDRB = (1 << PWM_PIN);  // | (1 << GREEN_PIN) | (1 << RED_PIN);
	#endif
	//PINB = PINB | (1 << GREEN_PIN);

	// Set timer to do PWM for correct output pin and set prescaler timing
	//TCCR0A = 0x23; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
	TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)

	// Turn features on or off as needed
	#ifdef VOLTAGE_MON
	ADC_on();
	#else
	ADC_off();
	#endif
	ACSR   |=  (1<<7); //AC off

	#ifdef BLINK_ON_POWER
	// blink once to let the user know we have power
	#ifdef ALT_MODES
	TCCR0A = FAST | 0b10100000;  // Use both outputs
	#else
	TCCR0A = FAST | 0b00100000;  // Only use the normal output
	#endif
	PWM_LVL = 255;
	_delay_ms(2);
	PWM_LVL = 0;
	_delay_ms(1);
	#endif

	// Enable sleep mode set to Power Down that will be triggered by the sleep_mode() command.
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	sleep_until_switch_press();

	uint8_t last_mode_idx = 0;

	while(1) {
		// We will never leave this loop.  The WDT will interrupt to check for switch presses and 
		// will change the mode if needed.  If this loop detects that the mode has changed, run the
		// logic for that mode while continuing to check for a mode change.
		if (mode_idx != last_mode_idx) {
			// The WDT changed the mode.
			if (mode_idx > 0) {
				// No need to change the mode if we are just turning the light off
				// Check if the PWM mode is different
				if (mode_pwm[last_mode_idx] != mode_pwm[mode_idx]) {
					#ifdef ALT_MODES
					TCCR0A = mode_pwm[mode_idx] | 0b10100000;  // Use both outputs
					#else
					TCCR0A = mode_pwm[mode_idx] | 0b00100000;  // Only use the normal output
					#endif
				}
			}
			PWM_LVL     = modes[mode_idx];
			#ifdef ALT_MODES
			ALT_PWM_LVL = alt_modes[mode_idx];
			#endif
			last_mode_idx = mode_idx;
			#ifdef ALT_MODES
			if (modes[mode_idx] == 0 && alt_modes[mode_idx] == 0) {
			#else
			if (mode_idx == 0) {
			#endif
				// Finish executing instructions for PWM level change
				// and/or voltage readout mode before shutdown.
				do {
					_delay_ms(1);
				} while (voltage_readout);
				// Go to sleep
				sleep_until_switch_press();
			}
		}
	}

    return 0; // Standard Return Code
}