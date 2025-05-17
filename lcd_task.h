#ifndef LCD_TASK_H
#define LCD_TASK_H

#include <os.h>
#include <stdint.h>
#include <stdbool.h>
#include "sensor_task.h"

// Task constants
#define LCD_TASK_PRIO              21u
#define LCD_TASK_STK_SIZE          256u

// Function prototypes
void lcd_task_init(void);
void lcd_task(void *p_arg);

// LCD helper functions
void lcd_init_display(void);
void lcd_update_display(sensor_data_t *sensor_data);
void lcd_update_measurement_interval_string(uint16_t interval_seconds);
void lcd_format_measurement_values(sensor_data_t *sensor_data, 
                                  char *co2_str, 
                                  char *temp_str, 
                                  char *humidity_str);

#endif // LCD_TASK_H
