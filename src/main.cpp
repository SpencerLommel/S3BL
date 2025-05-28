// Spencer Lommel
// May. 28th 2025
// S3BL (Spencer's Second Stage Bootloader) is a bootloader for the Teensy 4.0 microcontroller that supports OTA firmware updates over Ethernet.
// This is just built on top of the Arduino framework, this bootloader actually just lives at the default program entry point and we 
// do some initialization then jump to our application depending on where our metadata points to.
// 10% of the program space is reserved for the bootloader, the other 90% is divided into two partitions to support redundant firmware updates.
// There is also another small portion reserved for metadata storage.

#include <Arduino.h>
#include "imxrt.h"  // Teensy 4.0 specific header
#include <arm_math.h>

#define METADATA_ADDRESS 0x60031000
typedef struct {
   uint32_t active_slot;  // 0 = A, 1 = B
   uint32_t valid_a;
   uint32_t valid_b;
   uint32_t boot_count;
   uint32_t boot_success;
} boot_metadata_t;

#define SLOT_A_ADDRESS 0x60032000
#define SLOT_B_ADDRESS 0x60112000
#define NVIC_VTOR (*(volatile uint32_t *)0xE000ED08)

typedef void (*app_entry_t)(void);
boot_metadata_t* meta = (boot_metadata_t*)METADATA_ADDRESS;
void jump_to_app(uint32_t address) {
   // Disable interrupts before jumping
   __disable_irq();
   
   // Update vector table pointer using IMXRT specific register
   NVIC_VTOR = address;
   
   // Setup the stack pointer and jump
   uint32_t* vectorTable = (uint32_t*)address;
   uint32_t stack_pointer = vectorTable[0];
   uint32_t program_counter = vectorTable[1];
   
   // Set main stack pointer
   __asm__ volatile("MSR msp, %0" : : "r" (stack_pointer));
   
   // Jump to application
   asm volatile ("DSB");
   asm volatile ("ISB");
   ((void (*)(void))program_counter)();
}
void setup() {
   Serial.begin(115200);
   Serial.println("S3BL Bootloader Starting...");
   Serial.println(meta->active_slot);
   Serial.println("Valid A:");
   Serial.println(meta->valid_a);
  Serial.println("Valid B:");
   Serial.println(meta->valid_a);

  //  if (meta->active_slot == 0 && meta->valid_a) {
  //      jump_to_app(SLOT_A_ADDRESS);
  //      Serial.println("Jumped to application in slot A");
  //  } else if (meta->valid_b) {
  //      jump_to_app(SLOT_B_ADDRESS);
  //      Serial.println("Jumped to application in slot B");
  //  }
  //  Serial.println("Boot failed: fallback to bootloader");
  //  // Handle update server start or recovery
}
void loop() {
  // we might want to initialize Ethernet here
  // we can use either HTTP or TFTP for OTA firmware upload
}