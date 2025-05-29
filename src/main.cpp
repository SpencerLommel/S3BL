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
#include <Ethernet.h>
#include "flash.h"  // Add this include
#include <LittleFS.h>
#define PROG_FLASH_SIZE (1024 * 1024) // 1MB for metadata and future use
LittleFS_Program myfs;


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

void flash_erase_sector(uint32_t addr) {
    __disable_irq();
    
    // Set address
    IMXRT_FLEXSPI->IPCR0 = addr;
    
    // Write unlock sequence
    IMXRT_FLEXSPI->LUTKEY = FLEXSPI_LUT_KEY;
    IMXRT_FLEXSPI->LUTCR = FLEXSPI_LUT_UNLOCK;
    
    // Command for sector erase
    IMXRT_FLEXSPI->LUT[0] = 0x06000000; // Write enable
    IMXRT_FLEXSPI->LUT[1] = 0x20000000; // Sector erase
    
    // Execute write enable
    IMXRT_FLEXSPI->IPCMD = 1;
    while(IMXRT_FLEXSPI->INTR & 1) ; // Wait for completion
    IMXRT_FLEXSPI->INTR = 1;
    
    // Execute sector erase
    IMXRT_FLEXSPI->IPCMD = 2;
    while(IMXRT_FLEXSPI->INTR & 1) ; // Wait for completion
    IMXRT_FLEXSPI->INTR = 1;
    
    __enable_irq();
}

void flash_write(uint32_t addr, const void* data, size_t len) {
    Serial.println("Starting flash write...");
    uint32_t aligned_addr = addr & ~(SECTOR_SIZE - 1);
    
    // First erase the sector(s)
    Serial.println("Erasing sector...");
    flash_erase_sector(aligned_addr);
    
    Serial.println("Starting write process...");
    __disable_irq();
    
    // Write unlock sequence
    IMXRT_FLEXSPI->LUTKEY = FLEXSPI_LUT_KEY;
    IMXRT_FLEXSPI->LUTCR = FLEXSPI_LUT_UNLOCK;
    
    // Set up page program command
    IMXRT_FLEXSPI->LUT[0] = 0x06000000; // Write enable
    IMXRT_FLEXSPI->LUT[1] = 0x02000000; // Page program
    
    const uint32_t* src = (const uint32_t*)data;
    size_t words = (len + 3) / 4;
    
    for(size_t i = 0; i < words; i++) {
        // Write enable command
        IMXRT_FLEXSPI->IPCMD = 1;
        while(IMXRT_FLEXSPI->INTR & 1) ;
        IMXRT_FLEXSPI->INTR = 1;
        
        // Wait for flash ready and WIP bit to clear
        while(!(IMXRT_FLEXSPI->STS0 & 0x1)) ;
        
        // Write the data
        IMXRT_FLEXSPI->IPCR0 = addr + (i * 4);
        IMXRT_FLEXSPI->TFDR[0] = src[i];
        IMXRT_FLEXSPI->IPCMD = 2;
        while(IMXRT_FLEXSPI->INTR & 1) ;
        IMXRT_FLEXSPI->INTR = 1;
        
        // Wait for flash ready and WIP bit to clear
        while(!(IMXRT_FLEXSPI->STS0 & 0x1)) ;
        
        // Add a small delay to ensure the write is complete
        for(volatile int j = 0; j < 1000; j++) ;
    }
    
    __enable_irq();
    Serial.println("Write complete, verifying...");
    
    // Add a delay before verification
    delay(10);
    
    // Verify the write
    const uint32_t* written = (const uint32_t*)addr;
    bool verify_failed = false;
    for(size_t i = 0; i < words; i++) {
        if(written[i] != src[i]) {
            Serial.print("Flash write verification failed at word ");
            Serial.print(i);
            Serial.print(" Expected: 0x");
            Serial.print(src[i], HEX);
            Serial.print(" Got: 0x");
            Serial.println(written[i], HEX);
            verify_failed = true;
            break;
        }
    }
    
    if (!verify_failed) {
        Serial.println("Flash write verification successful!");
    }
}

void save_metadata(const boot_metadata_t& meta_data) {
    File f = myfs.open("/meta.bin", FILE_WRITE);
    if (f) {
        f.write((const uint8_t*)&meta_data, sizeof(meta_data));
        f.close();
        Serial.println("Metadata written to LittleFS_Program.");
    } else {
        Serial.println("Failed to open meta.bin for writing!");
    }
}

bool load_metadata(boot_metadata_t& meta_data) {
    File f = myfs.open("/meta.bin", FILE_READ);
    if (f && f.size() == sizeof(meta_data)) {
        f.read((uint8_t*)&meta_data, sizeof(meta_data));
        f.close();
        Serial.println("Metadata loaded from LittleFS_Program.");
        return true;
    }
    Serial.println("No valid metadata found in LittleFS_Program.");
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("S3BL Bootloader Starting...");
    delay(10);
    Serial.println("Checking metadata...");
    delay(10);
    if (!myfs.begin(PROG_FLASH_SIZE)) {
        Serial.println("Error starting PROGRAM FLASH DISK");
        while (1);
    }
    boot_metadata_t init_meta;
    if (!load_metadata(init_meta) || init_meta.active_slot == 0xFFFFFFFF) {
        Serial.println("Initializing metadata...");
        delay(10);
        init_meta.active_slot = 0;
        init_meta.valid_a = 0;
        init_meta.valid_b = 0;
        init_meta.boot_count = 0;
        init_meta.boot_success = 0;
        Serial.println("Writing metadata...");
        delay(10);
        save_metadata(init_meta);
        Serial.println("Verifying metadata...");
        delay(10);
        boot_metadata_t verify_meta;
        if (load_metadata(verify_meta) && verify_meta.active_slot == 0) {
            Serial.println("Metadata write successful!");
        } else {
            Serial.println("Metadata write failed!");
        }
    }
    Serial.println("Current metadata state:");
    delay(10);
    Serial.print("Active slot: 0x");
    Serial.println(init_meta.active_slot, HEX);
    Serial.print("Valid A: 0x");
    Serial.println(init_meta.valid_a, HEX);
    Serial.print("Valid B: 0x");
    Serial.println(init_meta.valid_b, HEX);
    delay(1000);

    // Boot decision logic
    if (init_meta.active_slot == 0 && init_meta.valid_a) {
        Serial.println("Jumping to application in slot A");
        jump_to_app(SLOT_A_ADDRESS);
    } else if (init_meta.active_slot == 1 && init_meta.valid_b) {
        Serial.println("Jumping to application in slot B");
        jump_to_app(SLOT_B_ADDRESS);
    } else if (init_meta.valid_a) {
        Serial.println("Active slot invalid, but slot A is valid. Jumping to slot A.");
        jump_to_app(SLOT_A_ADDRESS);
    } else if (init_meta.valid_b) {
        Serial.println("Active slot invalid, but slot B is valid. Jumping to slot B.");
        jump_to_app(SLOT_B_ADDRESS);
    } else {
        Serial.println("No valid application found. Entering recovery mode.");
        // Initialize Ethernet for recovery
        byte mac[6] = { 0x04, 0xE9, 0xE5, 0x00, 0x00, 0x01 };
        IPAddress ip(192, 168, 1, 222);
        IPAddress gateway(192, 168, 1, 1);
        IPAddress subnet(255, 255, 255, 0);
        Ethernet.begin(mac);
        Serial.print("Ethernet started. IP address: ");
        Serial.println(Ethernet.localIP());
        EthernetServer server(80);
        server.begin();
        Serial.println("Recovery HTTP server started on port 80");
        while (true) {
            EthernetClient client = server.available();
            if (client) {
                Serial.println("Client connected in recovery mode");
                String request = "";
                unsigned long timeout = millis() + 1000;
                while (client.connected() && millis() < timeout) {
                    while (client.available()) {
                        char c = client.read();
                        request += c;
                        timeout = millis() + 1000;
                    }
                }
                // Check for file upload POST
                if (request.indexOf("POST /upload") >= 0) {
                    int bodyStart = request.indexOf("\r\n\r\n");
                    if (bodyStart > 0) {
                        String code = request.substring(bodyStart + 4);
                        Serial.println("--- Received uploaded code ---");
                        Serial.println(code);
                        Serial.println("-----------------------------");
                        // Write to non-primary partition (slot B if active is A, else slot A)
                        uint32_t target_addr = (init_meta.active_slot == 0) ? SLOT_B_ADDRESS : SLOT_A_ADDRESS;
                        Serial.print("Writing uploaded code to partition at address 0x");
                        Serial.println(target_addr, HEX);
                        // Simulate writing code as binary to the partition (in real use, would parse and flash binary)
                        flash_erase_sector(target_addr);
                        flash_write(target_addr, code.c_str(), code.length());
                        Serial.println("Code written to flash partition.");
                        // Update metadata: set new slot as valid and active, invalidate the other
                        if (init_meta.active_slot == 0) {
                            init_meta.valid_b = 1;
                            init_meta.active_slot = 1;
                            init_meta.valid_a = 0; // Optionally invalidate A
                        } else {
                            init_meta.valid_a = 1;
                            init_meta.active_slot = 0;
                            init_meta.valid_b = 0; // Optionally invalidate B
                        }
                        save_metadata(init_meta);
                        Serial.println("Metadata updated. Rebooting to new application...");
                        // Respond to client
                        client.println("HTTP/1.1 200 OK");
                        client.println("Content-Type: text/plain");
                        client.println("Connection: close");
                        client.println();
                        client.println("Upload received. Code written to partition. Rebooting...");
                        client.stop();
                        delay(100);
                        SCB_AIRCR = 0x05FA0004; // Request system reset (ARM Cortex-M7)
                        while (1); // Wait for reset
                    }
                }
                // Serve upload form for GET /
                if (request.startsWith("GET / ") || request.startsWith("GET /HTTP")) {
                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-Type: text/html");
                    client.println("Connection: close");
                    client.println();
                    client.println("<html><head><title>S3BL Recovery</title></head><body>");
                    client.println("<h2>S3BL Recovery Mode</h2>");
                    client.println("<form method='POST' action='/upload' enctype='text/plain'>");
                    client.println("<label>Paste code or upload .cpp file:</label><br>");
                    client.println("<textarea name='code' rows='16' cols='60'></textarea><br>");
                    client.println("<input type='file' name='file' accept='.cpp'><br>");
                    client.println("<input type='submit' value='Upload & Boot'>");
                    client.println("</form>");
                    client.println("</body></html>");
                    client.stop();
                    continue;
                }
                // Fallback: print request
                Serial.println("--- Received HTTP data ---");
                Serial.println(request);
                Serial.println("--------------------------");
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: text/plain");
                client.println("Connection: close");
                client.println();
                client.println("S3BL Recovery Mode: Data received. Check serial for content.");
                client.stop();
            }
            delay(10);
        }
    }
}
void loop() {
    // Just print a heartbeat message every few seconds
    Serial.println("Bootloader running...");
    delay(5000);
}