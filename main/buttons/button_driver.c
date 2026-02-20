#include "buttons/button_driver.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h" 
#include "ui/config_screen.h"

void  ESP32_Button_init(void){
  gpio_reset_pin(Button_PIN1);                        
  gpio_set_direction(Button_PIN1, GPIO_MODE_INPUT);   
  gpio_set_pull_mode(Button_PIN1, GPIO_PULLUP_ONLY);   
}
uint8_t Button_GPIO_Get_Level(int GPIO_PIN){                
  return (uint8_t)(gpio_get_level(GPIO_PIN));
}
void Timer_Callback(void *arg){                             
  button_ticks();                                       
}



struct Button BUTTON1;                   
PressEvent BOOT_KEY_State,PWR_KEY_State;                    
uint8_t Read_Button_GPIO_Level(uint8_t button_id)           
{
  if(!button_id)                        
    return (uint8_t)(gpio_get_level(Button_PIN1));
  return 0;
}
void Button_SINGLE_CLICK_Callback(void* btn){          
  struct Button *user_button = (struct Button *)btn;      
  if(user_button == &BUTTON1){                      
    BOOT_KEY_State = SINGLE_CLICK;                    
    if (config_screen_is_active()) {
      config_screen_scroll_down();
    }
  }
}
void Button_DOUBLE_CLICK_Callback(void* btn){              
  struct Button *user_button = (struct Button *)btn;        
  if(user_button == &BUTTON1){            
    BOOT_KEY_State = DOUBLE_CLICK;                
  }
}
void Button_LONG_PRESS_START_Callback(void* btn){        
  struct Button *user_button = (struct Button *)btn;    
  if(user_button == &BUTTON1){                      
    BOOT_KEY_State= LONG_PRESS_START;                
  }
}
void button_Init(void)
{
  ESP32_Button_init();   
  button_init(&BUTTON1, Read_Button_GPIO_Level, 0 , 0);  
  button_attach(&BUTTON1, SINGLE_CLICK, Button_SINGLE_CLICK_Callback);      
  button_attach(&BUTTON1, DOUBLE_CLICK, Button_DOUBLE_CLICK_Callback);          
  button_attach(&BUTTON1, LONG_PRESS_START, Button_LONG_PRESS_START_Callback); 

  const esp_timer_create_args_t clock_tick_timer_args = 
  {
    .callback = &Timer_Callback,                                           
    .name = "Timer_task",                               
    .arg = NULL,
  };
  esp_timer_handle_t clock_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&clock_tick_timer_args, &clock_tick_timer));     
  ESP_ERROR_CHECK(esp_timer_start_periodic(clock_tick_timer, 1000 * 5));  
 
  BOOT_KEY_State = NONE_PRESS;              
  button_start(&BUTTON1);                                                   
}
