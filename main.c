/*
==============================================================================
DỰ ÁN: HỆ THỐNG GIÁM SÁT SỨC KHỎE ĐA CHỈ SỐ
MCU: STM32F411CEU6
Cảm biến: MAX30100 (Nhịp tim + SpO2) + DS18B20 (Nhiệt độ)
Hiển thị: OLED SSD1306 (SPI)
Debug: UART1 (PA9)
==============================================================================
*/

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ==================== KHAI BÁO HANDLE TOÀN CỤC ==================== */
I2C_HandleTypeDef hi2c1;  // I2C cho MAX30100
SPI_HandleTypeDef hspi1;  // SPI cho OLED
UART_HandleTypeDef huart1; // UART debug
TIM_HandleTypeDef htim2;   // Timer cho đọc nhịp tim

/* ==================== ĐỊNH NGHĨA CHÂN DS18B20 ==================== */
#define DS18B20_PORT        GPIOA
#define DS18B20_PIN         GPIO_PIN_1

/* ==================== ĐỊNH NGHĨA MAX30100 ==================== */
#define MAX30100_ADDR       0x57  // Địa chỉ I2C của MAX30100 (7-bit)

// Thanh ghi MAX30100
#define MAX30100_REG_INT_STATUS     0x00
#define MAX30100_REG_INT_ENABLE     0x01
#define MAX30100_REG_FIFO_WR_PTR    0x02
#define MAX30100_REG_FIFO_OVF_CTR   0x03
#define MAX30100_REG_FIFO_RD_PTR    0x04
#define MAX30100_REG_FIFO_DATA      0x05
#define MAX30100_REG_MODE_CONFIG    0x06
#define MAX30100_REG_SPO2_CONFIG    0x07
#define MAX30100_REG_LED_CONFIG     0x08
#define MAX30100_REG_TEMP_INTEGER   0x16
#define MAX30100_REG_TEMP_FRACTION  0x17
#define MAX30100_REG_REV_ID         0xFE
#define MAX30100_REG_PART_ID        0xFF

// Cấu hình chế độ
#define MAX30100_MODE_HR_ONLY       0x02  // Chỉ đo nhịp tim
#define MAX30100_MODE_SPO2_HR       0x03  // Đo cả SpO2 và nhịp tim

/* ==================== BIẾN TOÀN CỤC ==================== */
volatile float temperature = 0.0;
volatile int heartRate = 0;
volatile int spo2 = 0;
volatile uint8_t newDataReady = 0;

// Bộ lọc cho nhịp tim (Moving Average)
#define BUFFER_SIZE    10
int hrBuffer[BUFFER_SIZE];
int hrIndex = 0;
int hrSum = 0;

// Bộ lọc cho SpO2
int spo2Buffer[BUFFER_SIZE];
int spo2Index = 0;
int spo2Sum = 0;

/* ==================== PROTOTYPE FUNCTIONS ==================== */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_I2C1_Init(void);
void MX_SPI1_Init(void);
void MX_USART1_UART_Init(void);
void MX_TIM2_Init(void);

// DS18B20
void DS18B20_Init(void);
uint8_t DS18B20_Reset(void);
void DS18B20_Write(uint8_t data);
uint8_t DS18B20_Read(void);
float DS18B20_GetTemperature(void);

// MAX30100
uint8_t MAX30100_Init(void);
uint8_t MAX30100_ReadReg(uint8_t reg, uint8_t *data);
uint8_t MAX30100_WriteReg(uint8_t reg, uint8_t data);
void MAX30100_ReadFIFO(uint16_t *ir, uint16_t *red);
void MAX30100_ReadTemperature(float *temp);
void MAX30100_ProcessData(uint16_t ir, uint16_t red);

// OLED
void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t data);
void OLED_Init(void);
void OLED_Clear(void);
void OLED_SetCursor(uint8_t x, uint8_t y);
void OLED_WriteString(char *str);
void OLED_DisplayNumber(int num, uint8_t x, uint8_t y);
void OLED_DisplayFloat(float num, uint8_t decimal, uint8_t x, uint8_t y);

// Helper
void delay_us(uint32_t us);
void UART_SendString(char *str);

/* ==================== MAIN FUNCTION ==================== */
int main(void) {
    char displayBuffer[32];
    char uartBuffer[64];
    
    // Khởi tạo HAL
    HAL_Init();
    SystemClock_Config();
    
    // Khởi tạo các ngoại vi
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_TIM2_Init();
    
    // Khởi tạo cảm biến
    DS18B20_Init();
    OLED_Init();
    OLED_Clear();
    
    // Hiển thị thông báo khởi động
    OLED_WriteString("System Starting...");
    HAL_Delay(2000);
    OLED_Clear();
    
    // Khởi tạo MAX30100
    UART_SendString("Init MAX30100...\r\n");
    if (MAX30100_Init() != 1) {
        OLED_WriteString("MAX30100 Error!");
        UART_SendString("MAX30100 init FAILED!\r\n");
        while(1) { HAL_Delay(1000); } // Dừng nếu lỗi
    }
    UART_SendString("MAX30100 OK\r\n");
    
    // Bắt đầu timer đọc cảm biến (mỗi 10ms)
    HAL_TIM_Base_Start_IT(&htim2);
    
    UART_SendString("System Ready!\r\n");
    
    while (1) {
        if (newDataReady) {
            newDataReady = 0;
            
            // Đọc nhiệt độ từ DS18B20
            temperature = DS18B20_GetTemperature();
            
            // Cập nhật hiển thị OLED
            OLED_Clear();
            
            // Dòng 1: Nhiệt độ
            OLED_SetCursor(0, 0);
            OLED_WriteString("Temp: ");
            OLED_DisplayFloat(temperature, 1, 60, 0);
            OLED_WriteString("C");
            
            // Dòng 2: Nhịp tim
            OLED_SetCursor(0, 2);
            OLED_WriteString("HR:  ");
            OLED_DisplayNumber(heartRate, 40, 2);
            OLED_WriteString(" bpm");
            
            // Dòng 3: SpO2
            OLED_SetCursor(0, 4);
            OLED_WriteString("SpO2: ");
            OLED_DisplayNumber(spo2, 50, 4);
            OLED_WriteString("%");
            
            // Dòng 4: Trạng thái
            OLED_SetCursor(0, 6);
            if (heartRate > 0 && spo2 > 0) {
                OLED_WriteString("HR/SpO2 OK");
            } else {
                OLED_WriteString("Place finger!");
            }
            
            // Gửi dữ liệu qua UART
            sprintf(uartBuffer, "%.1f,%d,%d\r\n", temperature, heartRate, spo2);
            UART_SendString(uartBuffer);
        }
        
        HAL_Delay(100); // Giảm tải CPU
    }
}

/* ==================== DS18B20 DRIVER (1-WIRE) ==================== */
void DS18B20_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DS18B20_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DS18B20_PORT, &GPIO_InitStruct);
    HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
}

uint8_t DS18B20_Reset(void) {
    uint8_t presence = 0;
    
    // Kéo LOW ít nhất 480us
    HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_RESET);
    HAL_Delay(1); // 1ms = 1000us
    
    // Thả bus
    HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
    delay_us(60);
    
    // Đọc presence pulse
    if (HAL_GPIO_ReadPin(DS18B20_PORT, DS18B20_PIN) == GPIO_PIN_RESET) {
        presence = 1;
    }
    
    delay_us(480);
    return presence;
}

void DS18B20_Write(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_RESET);
        delay_us(2);
        
        if (data & (1 << i)) {
            HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
        }
        delay_us(60);
        HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
        delay_us(2);
    }
}

uint8_t DS18B20_Read(void) {
    uint8_t data = 0;
    
    for (int i = 0; i < 8; i++) {
        data >>= 1;
        HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_RESET);
        delay_us(2);
        HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
        delay_us(10);
        
        if (HAL_GPIO_ReadPin(DS18B20_PORT, DS18B20_PIN) == GPIO_PIN_SET) {
            data |= 0x80;
        }
        delay_us(50);
    }
    return data;
}

float DS18B20_GetTemperature(void) {
    uint8_t lsb, msb;
    int16_t raw;
    
    if (!DS18B20_Reset()) return -273.0;
    DS18B20_Write(0xCC); // Skip ROM
    DS18B20_Write(0x44); // Convert T
    HAL_Delay(750);
    
    if (!DS18B20_Reset()) return -273.0;
    DS18B20_Write(0xCC);
    DS18B20_Write(0xBE); // Read scratchpad
    
    lsb = DS18B20_Read();
    msb = DS18B20_Read();
    
    raw = (msb << 8) | lsb;
    return (float)raw / 16.0;
}

/* ==================== MAX30100 DRIVER (I2C) ==================== */
uint8_t MAX30100_Init(void) {
    uint8_t partId, revId;
    
    // Kiểm tra kết nối
    if (HAL_I2C_IsDeviceReady(&hi2c1, MAX30100_ADDR << 1, 3, 100) != HAL_OK) {
        return 0;
    }
    
    // Đọc Part ID (phải là 0x11)
    MAX30100_ReadReg(MAX30100_REG_PART_ID, &partId);
    if (partId != 0x11) {
        return 0;
    }
    
    // Reset cảm biến (ghi 0x40 vào MODE_CONFIG)
    MAX30100_WriteReg(MAX30100_REG_MODE_CONFIG, 0x40);
    HAL_Delay(100);
    
    // Cấu hình chế độ SpO2/HR
    MAX30100_WriteReg(MAX30100_REG_MODE_CONFIG, MAX30100_MODE_SPO2_HR);
    
    // Cấu hình SpO2: 100Hz, LED 1600uA, 16-bit ADC
    MAX30100_WriteReg(MAX30100_REG_SPO2_CONFIG, 0x47); // 100Hz, 4.7k, 16-bit
    
    // Cấu hình LED: 10mA cho IR, 10mA cho RED
    MAX30100_WriteReg(MAX30100_REG_LED_CONFIG, 0x2F); // 10mA
    
    // Bật ngắt
    MAX30100_WriteReg(MAX30100_REG_INT_ENABLE, 0xC0); // Ngắt khi có dữ liệu mới
    
    return 1;
}

uint8_t MAX30100_ReadReg(uint8_t reg, uint8_t *data) {
    return HAL_I2C_Mem_Read(&hi2c1, MAX30100_ADDR << 1, reg, 
                            I2C_MEMADD_SIZE_8BIT, data, 1, 100) == HAL_OK;
}

uint8_t MAX30100_WriteReg(uint8_t reg, uint8_t data) {
    return HAL_I2C_Mem_Write(&hi2c1, MAX30100_ADDR << 1, reg, 
                             I2C_MEMADD_SIZE_8BIT, &data, 1, 100) == HAL_OK;
}

void MAX30100_ReadFIFO(uint16_t *ir, uint16_t *red) {
    uint8_t buffer[4];
    HAL_I2C_Mem_Read(&hi2c1, MAX30100_ADDR << 1, MAX30100_REG_FIFO_DATA,
                     I2C_MEMADD_SIZE_8BIT, buffer, 4, 100);
    
    *ir = (buffer[0] << 8) | buffer[1];
    *red = (buffer[2] << 8) | buffer[3];
}

/* ==================== XỬ LÝ NHỊP TIM & SpO2 ==================== */
void MAX30100_ProcessData(uint16_t ir, uint16_t red) {
    static uint32_t lastBeatTime = 0;
    static uint32_t beatInterval = 0;
    static uint8_t beatDetected = 0;
    
    // Lọc giá trị IR (nhịp tim) - phát hiện đỉnh
    static uint16_t lastIr = 0;
    static uint16_t threshold = 500;
    
    // Phát hiện nhịp đập
    if (ir > threshold && lastIr <= threshold) {
        uint32_t now = HAL_GetTick();
        if (lastBeatTime > 0) {
            beatInterval = now - lastBeatTime;
            if (beatInterval > 300 && beatInterval < 1500) {
                heartRate = 60000 / beatInterval;
                
                // Lọc moving average cho nhịp tim
                hrSum = hrSum - hrBuffer[hrIndex] + heartRate;
                hrBuffer[hrIndex] = heartRate;
                hrIndex = (hrIndex + 1) % BUFFER_SIZE;
                heartRate = hrSum / BUFFER_SIZE;
                
                beatDetected = 1;
            }
        }
        lastBeatTime = now;
    }
    lastIr = ir;
    
    // Tính SpO2 đơn giản từ tỷ lệ RED/IR
    // Công thức: SpO2 = 104 - 17 * (RED/IR) (gần đúng)
    if (ir > 0 && beatDetected) {
        float ratio = (float)red / ir;
        int rawSpO2 = (int)(104 - (17 * ratio));
        
        if (rawSpO2 > 100) rawSpO2 = 100;
        if (rawSpO2 < 70) rawSpO2 = 70;
        
        // Lọc moving average cho SpO2
        spo2Sum = spo2Sum - spo2Buffer[spo2Index] + rawSpO2;
        spo2Buffer[spo2Index] = rawSpO2;
        spo2Index = (spo2Index + 1) % BUFFER_SIZE;
        spo2 = spo2Sum / BUFFER_SIZE;
    }
    
    // Tự động điều chỉnh ngưỡng
    if (ir > threshold + 200) threshold = ir - 100;
    if (ir < threshold - 200) threshold = ir + 100;
    if (threshold < 200) threshold = 200;
    if (threshold > 2000) threshold = 2000;
}

/* ==================== OLED DRIVER (SPI SOFTWARE) ==================== */
#define OLED_SCK_PIN    GPIO_PIN_5
#define OLED_SDA_PIN    GPIO_PIN_7
#define OLED_RES_PIN    GPIO_PIN_2
#define OLED_DC_PIN     GPIO_PIN_3
#define OLED_CS_PIN     GPIO_PIN_4
#define OLED_PORT       GPIOA

void SPI_WriteByte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        if (data & 0x80) {
            HAL_GPIO_WritePin(OLED_PORT, OLED_SDA_PIN, GPIO_PIN_SET);
        } else {
            HAL_GPIO_WritePin(OLED_PORT, OLED_SDA_PIN, GPIO_PIN_RESET);
        }
        
        HAL_GPIO_WritePin(OLED_PORT, OLED_SCK_PIN, GPIO_PIN_SET);
        delay_us(1);
        HAL_GPIO_WritePin(OLED_PORT, OLED_SCK_PIN, GPIO_PIN_RESET);
        data <<= 1;
    }
}

void OLED_WriteCommand(uint8_t cmd) {
    HAL_GPIO_WritePin(OLED_PORT, OLED_DC_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(OLED_PORT, OLED_CS_PIN, GPIO_PIN_RESET);
    SPI_WriteByte(cmd);
    HAL_GPIO_WritePin(OLED_PORT, OLED_CS_PIN, GPIO_PIN_SET);
}

void OLED_WriteData(uint8_t data) {
    HAL_GPIO_WritePin(OLED_PORT, OLED_DC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(OLED_PORT, OLED_CS_PIN, GPIO_PIN_RESET);
    SPI_WriteByte(data);
    HAL_GPIO_WritePin(OLED_PORT, OLED_CS_PIN, GPIO_PIN_SET);
}

void OLED_Init(void) {
    // Reset OLED
    HAL_GPIO_WritePin(OLED_PORT, OLED_RES_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(OLED_PORT, OLED_RES_PIN, GPIO_PIN_SET);
    HAL_Delay(100);
    
    // Init sequence
    OLED_WriteCommand(0xAE); // Off
    OLED_WriteCommand(0xD5); OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xA8); OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xD3); OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0x8D); OLED_WriteCommand(0x14);
    OLED_WriteCommand(0x20); OLED_WriteCommand(0x00);
    OLED_WriteCommand(0xA1);
    OLED_WriteCommand(0xC8);
    OLED_WriteCommand(0xDA); OLED_WriteCommand(0x12);
    OLED_WriteCommand(0x81); OLED_WriteCommand(0xCF);
    OLED_WriteCommand(0xD9); OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDB); OLED_WriteCommand(0x40);
    OLED_WriteCommand(0xA4);
    OLED_WriteCommand(0xA6);
    OLED_WriteCommand(0xAF); // On
}

void OLED_Clear(void) {
    for (int page = 0; page < 8; page++) {
        OLED_WriteCommand(0xB0 + page);
        OLED_WriteCommand(0x00);
        OLED_WriteCommand(0x10);
        for (int col = 0; col < 128; col++) {
            OLED_WriteData(0x00);
        }
    }
}

void OLED_SetCursor(uint8_t x, uint8_t y) {
    OLED_WriteCommand(0xB0 + y);      // Page
    OLED_WriteCommand(x & 0x0F);      // Lower column
    OLED_WriteCommand(0x10 | (x >> 4)); // Upper column
}

// Font 8x8 đơn giản
const uint8_t font8x8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x00,0x00,0x5F,0x00,0x00,0x00,0x00,0x00}, // !
    // ... (thêm các ký tự cần thiết)
    // Chữ cái cơ bản
    {0x3E,0x51,0x49,0x45,0x3E,0x00,0x00,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00,0x00,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46,0x00,0x00,0x00}, // 2
    {0x21,0x41,0x45,0x4B,0x31,0x00,0x00,0x00}, // 3
    {0x18,0x14,0x12,0x7F,0x10,0x00,0x00,0x00}, // 4
    {0x27,0x45,0x45,0x45,0x39,0x00,0x00,0x00}, // 5
    {0x3C,0x4A,0x49,0x49,0x30,0x00,0x00,0x00}, // 6
    {0x01,0x71,0x09,0x05,0x03,0x00,0x00,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, // 8
    {0x06,0x49,0x49,0x29,0x1E,0x00,0x00,0x00}, // 9
};

void OLED_WriteChar(char c) {
    if (c >= '0' && c <= '9') {
        for (int i = 0; i < 8; i++) {
            OLED_WriteData(font8x8[c - '0'][i]);
        }
    } else {
        for (int i = 0; i < 8; i++) {
            OLED_WriteData(0x00);
        }
    }
}

void OLED_WriteString(char *str) {
    while (*str) {
        OLED_WriteChar(*str++);
    }
}

void OLED_DisplayNumber(int num, uint8_t x, uint8_t y) {
    char buffer[10];
    sprintf(buffer, "%d", num);
    OLED_SetCursor(x, y);
    OLED_WriteString(buffer);
}

void OLED_DisplayFloat(float num, uint8_t decimal, uint8_t x, uint8_t y) {
    char buffer[20];
    sprintf(buffer, "%.*f", decimal, num);
    OLED_SetCursor(x, y);
    OLED_WriteString(buffer);
}

/* ==================== HELPER FUNCTIONS ==================== */
void delay_us(uint32_t us) {
    uint32_t cycles = us * (SystemCoreClock / 1000000);
    while (cycles--) {
        __NOP();
    }
}

void UART_SendString(char *str) {
    HAL_UART_Transmit(&huart1, (uint8_t*)str, strlen(str), 100);
}

/* ==================== TIMER INTERRUPT ==================== */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM2) {
        static uint8_t sampleCount = 0;
        sampleCount++;
        
        // Đọc FIFO MAX30100 mỗi 10ms (100Hz)
        if (sampleCount >= 1) {
            sampleCount = 0;
            uint16_t ir, red;
            MAX30100_ReadFIFO(&ir, &red);
            MAX30100_ProcessData(ir, red);
            newDataReady = 1;
        }
    }
}

/* ==================== CÁC HÀM KHỞI TẠO (CubeMX sẽ sinh ra) ==================== */
// Các hàm này sẽ được STM32CubeMX tự động sinh ra khi bạn cấu hình:
// - I2C1 (PB6, PB7)
// - SPI1 (PA5, PA7)
// - USART1 (PA9)
// - TIM2 (50Hz)
// Bạn copy phần code CubeMX sinh ra vào đây.

void SystemClock_Config(void) {
    // Code do CubeMX sinh ra
}

void MX_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    
    // PA1 DS18B20
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    
    // PA9 UART TX
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    
    // PB6 I2C SCL, PB7 I2C SDA
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    
    // OLED pins
    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void MX_I2C1_Init(void) {
    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

void MX_USART1_UART_Init(void) {
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

void MX_TIM2_Init(void) {
    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 8399;  // 84MHz / 8400 = 10kHz
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 99;        // 10kHz / 100 = 100Hz
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    HAL_TIM_Base_Init(&htim2);
}
