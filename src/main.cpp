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
        // Print vector table for diagnostics
        Serial.println("Slot A vector table (first 32 bytes):");
        for (int i = 0; i < 8; i++) {
            uint32_t word = *((uint32_t*)(SLOT_A_ADDRESS + i * 4));
            Serial.print("0x"); Serial.print(word, HEX); Serial.print(" ");
        }
        Serial.println();
        // Check vector table validity
        uint32_t sp = *((uint32_t*)SLOT_A_ADDRESS);
        uint32_t rv = *((uint32_t*)(SLOT_A_ADDRESS + 4));
        if ((sp & 0x60000000) != 0x60000000 || (rv & 0x60000000) != 0x60000000) {
            Serial.println("WARNING: Slot A does not appear to contain a valid ARM Cortex-M7 binary. Aborting jump.");
        } else {
            jump_to_app(SLOT_A_ADDRESS);
        }
    } else if (init_meta.active_slot == 1 && init_meta.valid_b) {
        Serial.println("Jumping to application in slot B");
        Serial.println("Slot B vector table (first 32 bytes):");
        for (int i = 0; i < 8; i++) {
            uint32_t word = *((uint32_t*)(SLOT_B_ADDRESS + i * 4));
            Serial.print("0x"); Serial.print(word, HEX); Serial.print(" ");
        }
        Serial.println();
        uint32_t sp = *((uint32_t*)SLOT_B_ADDRESS);
        uint32_t rv = *((uint32_t*)(SLOT_B_ADDRESS + 4));
        if ((sp & 0x60000000) != 0x60000000 || (rv & 0x60000000) != 0x60000000) {
            Serial.println("WARNING: Slot B does not appear to contain a valid ARM Cortex-M7 binary. Aborting jump.");
        } else {
            jump_to_app(SLOT_B_ADDRESS);
        }
    } else if (init_meta.valid_a) {
        Serial.println("Active slot invalid, but slot A is valid. Jumping to slot A.");
        Serial.println("Slot A vector table (first 32 bytes):");
        for (int i = 0; i < 8; i++) {
            uint32_t word = *((uint32_t*)(SLOT_A_ADDRESS + i * 4));
            Serial.print("0x"); Serial.print(word, HEX); Serial.print(" ");
        }
        Serial.println();
        uint32_t sp = *((uint32_t*)SLOT_A_ADDRESS);
        uint32_t rv = *((uint32_t*)(SLOT_A_ADDRESS + 4));
        if ((sp & 0x60000000) != 0x60000000 || (rv & 0x60000000) != 0x60000000) {
            Serial.println("WARNING: Slot A does not appear to contain a valid ARM Cortex-M7 binary. Aborting jump.");
        } else {
            jump_to_app(SLOT_A_ADDRESS);
        }
    } else if (init_meta.valid_b) {
        Serial.println("Active slot invalid, but slot B is valid. Jumping to slot B.");
        Serial.println("Slot B vector table (first 32 bytes):");
        for (int i = 0; i < 8; i++) {
            uint32_t word = *((uint32_t*)(SLOT_B_ADDRESS + i * 4));
            Serial.print("0x"); Serial.print(word, HEX); Serial.print(" ");
        }
        Serial.println();
        uint32_t sp = *((uint32_t*)SLOT_B_ADDRESS);
        uint32_t rv = *((uint32_t*)(SLOT_B_ADDRESS + 4));
        if ((sp & 0x60000000) != 0x60000000 || (rv & 0x60000000) != 0x60000000) {
            Serial.println("WARNING: Slot B does not appear to contain a valid ARM Cortex-M7 binary. Aborting jump.");
        } else {
            jump_to_app(SLOT_B_ADDRESS);
        }
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
                unsigned long start_time = millis();
                // Read the first line (request line)
                while (client.connected() && client.available() == 0 && millis() - start_time < 1000) {
                    delay(1);
                }
                // Read the request line
                String req_line = "";
                while (client.connected() && client.available()) {
                    char c = client.read();
                    if (c == '\n') break;
                    if (c != '\r') req_line += c;
                }
                Serial.print("HTTP request line: ");
                Serial.println(req_line);
                // Only buffer and process upload for POST /upload
                if (req_line.startsWith("POST /upload")) {
                    // Read headers until blank line
                    String headers = "";
                    int content_length = 0;
                    while (client.connected()) {
                        String line = "";
                        while (client.available()) {
                            char c = client.read();
                            if (c == '\n') break;
                            if (c != '\r') line += c;
                        }
                        if (line.length() == 0) break; // End of headers
                        headers += line + "\n";
                        if (line.startsWith("Content-Length:")) {
                            content_length = line.substring(15).toInt();
                        }
                    }
                    Serial.print("Content-Length: "); Serial.println(content_length);
                    // Now read the body (uploaded file)
                    size_t upload_bytes = 0;
                    bool upload_too_large = false;
                    const size_t MAX_UPLOAD_SIZE = 1024 * 1024; // 1MB
                    unsigned long timeout = millis() + 10000;
                    String code = "";
                    while (client.connected() && upload_bytes < content_length && millis() < timeout) {
                        while (client.available() && upload_bytes < content_length) {
                            char c = client.read();
                            code += c;
                            upload_bytes++;
                            if (upload_bytes % 1024 == 0) {
                                Serial.print("Upload progress: ");
                                Serial.print(upload_bytes);
                                Serial.println(" bytes received");
                            }
                            if (upload_bytes > MAX_UPLOAD_SIZE) {
                                Serial.println("ERROR: Uploaded file exceeds 1MB. Aborting upload.");
                                upload_too_large = true;
                                break;
                            }
                            timeout = millis() + 10000;
                        }
                        if (upload_too_large) break;
                    }
                    if (upload_too_large) {
                        client.println("HTTP/1.1 413 Payload Too Large");
                        client.println("Content-Type: text/plain");
                        client.println("Connection: close");
                        client.println();
                        client.println("ERROR: Uploaded file exceeds 1MB. Aborting upload.");
                        client.stop();
                        continue;
                    }
                    if (upload_bytes == 0) {
                        Serial.println("No data received from client.");
                    }
                    if (millis() >= timeout) {
                        Serial.println("ERROR: Upload timed out (no data for 10s). Aborting.");
                        client.println("HTTP/1.1 408 Request Timeout");
                        client.println("Content-Type: text/plain");
                        client.println("Connection: close");
                        client.println();
                        client.println("ERROR: Upload timed out (no data for 10s). Aborting.");
                        client.stop();
                        continue;
                    }
                    Serial.println("--- Received uploaded code ---");
                    Serial.println(code);
                    Serial.println("-----------------------------");
                    // Write to non-primary partition (slot B if active is A, else slot A)
                    uint32_t target_addr = (init_meta.active_slot == 0) ? SLOT_B_ADDRESS : SLOT_A_ADDRESS;
                    // Parse multipart/form-data to extract the binary payload
                    int bin_start = -1, bin_end = -1;
                    // Find the start of the binary (after the first double CRLF after Content-Type)
                    int content_type_idx = code.indexOf("Content-Type:");
                    if (content_type_idx >= 0) {
                        int bin_hdr_end = code.indexOf("\r\n\r\n", content_type_idx);
                        if (bin_hdr_end >= 0) {
                            bin_start = bin_hdr_end + 4;
                            // Find the boundary at the end
                            String boundary = code.substring(0, code.indexOf("\r\n"));
                            bin_end = code.indexOf(boundary, bin_start) - 4; // -4 to remove trailing CRLF
                        }
                    }
                    if (bin_start >= 0 && bin_end > bin_start) {
                        Serial.print("Extracted binary payload: start=");
                        Serial.print(bin_start);
                        Serial.print(", end=");
                        Serial.println(bin_end);
                        int bin_len = bin_end - bin_start;
                        // Write only the binary payload to flash
                        flash_erase_sector(target_addr);
                        flash_write(target_addr, code.c_str() + bin_start, bin_len);
                        Serial.print("Wrote "); Serial.print(bin_len); Serial.println(" bytes of firmware to flash partition.");
                    } else {
                        Serial.println("ERROR: Could not parse firmware binary from multipart upload. Aborting.");
                        client.println("HTTP/1.1 400 Bad Request");
                        client.println("Content-Type: text/plain");
                        client.println("Connection: close");
                        client.println();
                        client.println("ERROR: Could not parse firmware binary from upload. Make sure you are uploading a .bin file.");
                        client.stop();
                        continue;
                    }
                    Serial.println("Code written to flash partition.");
                    // Update metadata: set new slot as valid and active, invalidate the other
                    if (init_meta.active_slot == 0) {
                        init_meta.valid_b = 1;
                        init_meta.active_slot = 1;
                        init_meta.valid_a = 0;
                    } else {
                        init_meta.valid_a = 1;
                        init_meta.active_slot = 0;
                        init_meta.valid_b = 0;
                    }
                    save_metadata(init_meta);
                    Serial.println("Metadata updated. Rebooting to new application...");
                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-Type: text/plain");
                    client.println("Connection: close");
                    client.println();
                    client.println("Upload received. Code written to partition. Rebooting...");
                    client.stop();
                    delay(100);
                    SCB_AIRCR = 0x05FA0004;
                    while (1);
                } else if (req_line.startsWith("GET / ") || req_line.startsWith("GET /HTTP")) {
                    // Serve upload form for GET /
                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-Type: text/html");
                    client.println("Connection: close");
                    client.println();
                    client.println("<html><head><title>S3BL Recovery</title></head><body>");
                    client.println("<h2>S3BL Recovery Mode</h2>");
                    client.println("<h2>Upload Compiled Firmware (.bin)</h2>");
                    client.println("<p style='color:red'><b>NOTE:</b> Only compiled binary files (.bin) generated for Teensy 4.0 are supported. Do NOT upload C++ source code. The file must start with a valid ARM Cortex-M7 vector table.</p>");
                    client.println("<form method='POST' action='/upload' enctype='multipart/form-data'>");
                    client.println("<input type='file' name='firmware' accept='.bin'><br><br>");
                    client.println("<input type='submit' value='Upload Firmware'>");
                    client.println("</form>");
                    client.println("<hr>");
                    client.println("<h3>Advanced: Upload Raw Code (NOT SUPPORTED)</h3>");
                    client.println("<p style='color:orange'>Uploading C++ code as text will NOT work. Only compiled .bin files are supported.</p>");
                    client.println("<form method='POST' action='/upload' enctype='text/plain'>");
                    client.println("<textarea name='code' rows='16' cols='60'></textarea><br>");
                    client.println("<input type='submit' value='Upload Code (Not Supported)'>");
                    client.println("</form>");
                    client.println("</body></html>");
                    client.stop();
                    continue;
                } else {
                    // Fallback: print request
                    Serial.println("--- Received HTTP data ---");
                    Serial.println(req_line);
                    Serial.println("--------------------------");
                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-Type: text/plain");
                    client.println("Connection: close");
                    client.println();
                    client.println("S3BL Recovery Mode: Data received. Check serial for content.");
                    client.stop();
                }
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