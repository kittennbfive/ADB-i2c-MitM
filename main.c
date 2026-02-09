#include <avr/io.h>
#include <stdint.h>
#include <stdbool.h>
#include <avr/interrupt.h>
#include <util/delay.h>

/*
This code does kind of a "man in the middle" attack for modifying I2C transactions on the fly. It was tailored to a specific task for hacking an old TV.

Copyright (c) 2026 by kittennbfive - https://github.com/kittennbfive/ADB-i2c-MitM

GPLv3+ and NO WARRANTY!

Please read the fine manual!
*/

#define DDR_I2C_MASTER DDRD
#define PORT_I2C_MASTER PORTD
#define PIN_I2C_MASTER PIND
#define SCL_MASTER PD2 //pin "D2" on Arduino Nano
#define SDA_MASTER PD3 //pin "D3" on Arduino Nano

#define DDR_I2C_TARGET DDRB
#define PORT_I2C_TARGET PORTB
#define PIN_I2C_TARGET PINB
#define SCL_TARGET PB1 //pin "D9" on Arduino Nano - don't forget the external pullup!
#define SDA_TARGET PB0 //pin "D8" on Arduino Nano - don't forget the external pullup!

#define PORT_INJECT_BIT_VALUE PORTD
#define PIN_INJECT_BIT_VALUE PIND
#define INJECT_BIT PD5 //pin "D5" on Arduino Nano - has *internal* pullup

#define TARGET_ADDR 0x88 //LSB must be 0
#define TARGET_SUB_ADDR 0x00
#define TARGET_BIT 7

#define DELAY_US_ACK_NACK 3 //this might need tweaking

#define DELAY_US_I2C_TIMEOUT 7500UL //this might need tweaking

//--- do not change anything below this line for normal use ---

#define MASTER_SCL_VALUE (!!(PIN_I2C_MASTER&(1<<SCL_MASTER)))
#define MASTER_SDA_VALUE (!!(PIN_I2C_MASTER&(1<<SDA_MASTER)))
#define TARGET_SDA_VALUE (!!(PIN_I2C_TARGET&(1<<SDA_TARGET)))
//emulate open collector
#define SET_MASTER_SDA_HIGH DDR_I2C_MASTER&=~(1<<SDA_MASTER)
#define SET_MASTER_SDA_LOW DDR_I2C_MASTER|=(1<<SDA_MASTER)
#define SET_TARGET_SDA_HIGH DDR_I2C_TARGET&=~(1<<SDA_TARGET)
#define SET_TARGET_SDA_LOW DDR_I2C_TARGET|=(1<<SDA_TARGET)
#define SET_TARGET_SCL_HIGH DDR_I2C_TARGET&=~(1<<SCL_TARGET)
#define SET_TARGET_SCL_LOW DDR_I2C_TARGET|=(1<<SCL_TARGET)

#define INJECT_BIT_VALUE (!!(PIN_INJECT_BIT_VALUE&(1<<INJECT_BIT)))

#define TIMEOUT_TIMER_COMPARE_VALUE (2*DELAY_US_I2C_TIMEOUT) //for timer 1 with prescaler 2
#define STOP_TIMER TCCR1B=0

//valid for Arduino Nano
#define DDR_BUILTIN_LED DDRB
#define PORT_BUILTIN_LED PORTB
#define PIN_BUILTIN_LED PINB
#define BUILTIN_LED PB5

#define LED_ON PORT_BUILTIN_LED|=(1<<BUILTIN_LED)
#define LED_OFF PORT_BUILTIN_LED&=~(1<<BUILTIN_LED)
#define LED_TOGGLE PIN_BUILTIN_LED|=(1<<BUILTIN_LED)

typedef enum
{
	STATE_IDLE,
	STATE_ADDRESS_ON_WIRE,
	STATE_SUBADDRESS_ON_WIRE,
	STATE_DATA_ON_WIRE,
	STATE_WAIT_FOR_STOP
} state_t;

static volatile bool timeout_occured=false;

//we need speed so inline all the things!

//clock edge detection in software is not reliable, use the internal interrupt hardware of the ATmega328P
__attribute__((__always_inline__)) static inline void wait_for_rising_clock_edge(void)
{
	EICRA=(1<<ISC01)|(1<<ISC00);
	EIFR|=(1<<INTF0);
	while(!(EIFR&(1<<INTF0)))
	{
		if(timeout_occured)
			break;
	}
	EIFR|=(1<<INTF0);
}

__attribute__((__always_inline__)) static inline void wait_for_falling_clock_edge(void)
{
	EICRA=(1<<ISC01);
	EIFR|=(1<<INTF0);
	while(!(EIFR&(1<<INTF0)))
	{
		if(timeout_occured)
			break;
	}
	EIFR|=(1<<INTF0);
}

__attribute__((__always_inline__)) static inline void wait_for_i2c_start(void)
{
	EICRA=(1<<ISC11);
	EIFR|=(1<<INTF1);
	bool found=false;
	while(!found)
	{
		while(!(EIFR&(1<<INTF1)))
		{
			if(timeout_occured)
				break;
		}
		if(MASTER_SCL_VALUE)
			found=true;
		EIFR|=(1<<INTF1);
	}

	//propagate start to target I2C bus
	SET_TARGET_SDA_LOW;

	//reset and start timer 1 for timeout stuff
	TCNT1=0;
	TCCR1B=(1<<WGM12)|(1<<CS11);
}

__attribute__((__always_inline__)) static inline void wait_for_i2c_stop(void)
{
	EICRA=(1<<ISC11)|(1<<ISC10);
	EIFR|=(1<<INTF1);
	bool found=false;
	while(!found)
	{
		while(!(EIFR&(1<<INTF1)))
		{
			if(timeout_occured)
				break;
		}
		if(MASTER_SCL_VALUE)
			found=true;
		EIFR|=(1<<INTF1);
	}
	
	//propagate start to target I2C bus
	SET_TARGET_SDA_LOW;
	_delay_us(DELAY_US_ACK_NACK);
	SET_TARGET_SCL_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	SET_TARGET_SDA_HIGH;

	//stop timer 1
	STOP_TIMER;
}

__attribute__((__always_inline__)) static inline bool receive_addr(const uint8_t target_addr)
{
	SET_TARGET_SCL_LOW;
	
	uint8_t addr=0;
	uint8_t i;
	for(i=0; i<8; i++)
	{
		if(timeout_occured)
			return false;
		addr<<=1;
		wait_for_rising_clock_edge();
		//propagate
		if(MASTER_SDA_VALUE)
			SET_TARGET_SDA_HIGH;
		else
			SET_TARGET_SDA_LOW;
		SET_TARGET_SCL_HIGH;
		addr|=MASTER_SDA_VALUE;
		wait_for_falling_clock_edge();
		SET_TARGET_SCL_LOW;
	}

	//get (N)ACK from target 
	SET_TARGET_SDA_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	SET_TARGET_SCL_HIGH;
	_delay_us(DELAY_US_ACK_NACK);

	bool ret=false;

	if(addr==target_addr)
	{
		//propagate (N)ACK from target back to master
		if(TARGET_SDA_VALUE)
			SET_MASTER_SDA_HIGH;
		else
			SET_MASTER_SDA_LOW;
		ret=true;
	}

	//(N)ACK clock pulse from master
	wait_for_rising_clock_edge();
	wait_for_falling_clock_edge();
	SET_TARGET_SCL_LOW;
	SET_MASTER_SDA_HIGH; //release bus!

	return ret;
}

__attribute__((__always_inline__)) static inline uint8_t receive_byte(void)
{
	SET_TARGET_SCL_LOW;
	
	uint8_t byte=0;
	uint8_t i;
	for(i=0; i<8; i++)
	{
		if(timeout_occured)
			return 0x00;
		byte<<=1;
		wait_for_rising_clock_edge();
		//propagate
		if(MASTER_SDA_VALUE)
			SET_TARGET_SDA_HIGH;
		else
			SET_TARGET_SDA_LOW;
		SET_TARGET_SCL_HIGH;
		byte|=MASTER_SDA_VALUE;
		wait_for_falling_clock_edge();
		SET_TARGET_SCL_LOW;
	}

	//get (N)ACK from target 
	SET_TARGET_SDA_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	SET_TARGET_SCL_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	
	//propagate (N)ACK from target back to master
	if(TARGET_SDA_VALUE)
		SET_MASTER_SDA_HIGH;
	else
		SET_MASTER_SDA_LOW;

	//(N)ACK clock pulse from master
	wait_for_rising_clock_edge();
	wait_for_falling_clock_edge();
	SET_TARGET_SCL_LOW;
	SET_MASTER_SDA_HIGH; //release bus!

	return byte;
}

__attribute__((__always_inline__)) static inline void hack_data_on_wire(const bool do_mitm)
{
	bool bit;
	int8_t i;
	bool inject_bit=INJECT_BIT_VALUE;
	for(i=7; i>=0; i--)
	{
		if(timeout_occured)
			return;
		
		wait_for_rising_clock_edge();
		bit=MASTER_SDA_VALUE;

		if(do_mitm && i==TARGET_BIT)
		{
			if(inject_bit)
				SET_TARGET_SDA_HIGH;
			else
				SET_TARGET_SDA_LOW;
		}
		else
		{
			if(bit)
				SET_TARGET_SDA_HIGH;
			else
				SET_TARGET_SDA_LOW;
		}

		SET_TARGET_SCL_HIGH;
		wait_for_falling_clock_edge();
		SET_TARGET_SCL_LOW;
	}

	//get (N)ACK from target 
	SET_TARGET_SDA_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	SET_TARGET_SCL_HIGH;
	_delay_us(DELAY_US_ACK_NACK);
	
	//propagate (N)ACK from target back to master
	if(TARGET_SDA_VALUE)
		SET_MASTER_SDA_HIGH;
	else
		SET_MASTER_SDA_LOW;

	//(N)ACK clock pulse from master
	wait_for_rising_clock_edge();
	wait_for_falling_clock_edge();
	SET_TARGET_SCL_LOW;
	SET_MASTER_SDA_HIGH; //release bus!

}

ISR(TIMER1_COMPA_vect)
{
	timeout_occured=true;
}

static void timeout_blink(void)
{
	//blink builtin-LED several times to show that something went wrong (I2C timeout)
	uint8_t i;
	for(i=0; i<5; i++)
	{
		LED_ON;
		_delay_ms(100);
		LED_OFF;
		_delay_ms(400);
	}
}

int main(void)
{
	PORT_INJECT_BIT_VALUE|=(1<<INJECT_BIT); //enable internal pullup

	DDR_BUILTIN_LED|=(1<<BUILTIN_LED);
	LED_OFF;
	
	PORT_I2C_MASTER&=~((1<<SCL_MASTER)|(1<<SDA_MASTER));
	PORT_I2C_TARGET&=~((1<<SCL_TARGET)|(1<<SDA_TARGET));
	
	SET_MASTER_SDA_HIGH;

	SET_TARGET_SCL_HIGH;
	SET_TARGET_SDA_HIGH;

	state_t state=STATE_IDLE;
	bool do_mitm=false;

	OCR1A=TIMEOUT_TIMER_COMPARE_VALUE;
	TIMSK1=(1<<OCIE1A);

	sei();

	while(1)
	{
		if(timeout_occured)
		{
			timeout_occured=false;
			//release both bus
			SET_MASTER_SDA_HIGH;
			SET_TARGET_SCL_HIGH;
			SET_TARGET_SDA_HIGH;
			timeout_blink();
			state=STATE_IDLE;
		}
	
		switch(state)
		{
			case STATE_IDLE:
				wait_for_i2c_start();
				state=STATE_ADDRESS_ON_WIRE;
				break;

			case STATE_ADDRESS_ON_WIRE:
				if(receive_addr(TARGET_ADDR))
					state=STATE_SUBADDRESS_ON_WIRE;
				else
				{
					STOP_TIMER; //master is writing to / reading from another device, no need for timeout-detection and ignore everything until next STOP
					state=STATE_WAIT_FOR_STOP;
				}
				break;

			case STATE_SUBADDRESS_ON_WIRE:
				if(receive_byte()==TARGET_SUB_ADDR)
					do_mitm=true;
				else
					do_mitm=false;
				state=STATE_DATA_ON_WIRE;
				break;

			case STATE_DATA_ON_WIRE:
				hack_data_on_wire(do_mitm);
				state=STATE_WAIT_FOR_STOP;
				break;

			case STATE_WAIT_FOR_STOP:
				wait_for_i2c_stop();
				state=STATE_IDLE;
				break;
		}
		
	}
	
	return 0;
}
