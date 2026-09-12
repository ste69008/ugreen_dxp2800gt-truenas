#include <linux/types.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>

extern struct mutex ug_201x_lock;
extern void ArgbRgbSet(int ArgbId, int RgbDat);
extern void ArgbLightSet(int ArgbId,int Light);
extern void ArgbModeSet(int ArgbId, int Mode, int period, int duty);

#define LED_COUNT       4

#define  COLOR_NONE		0x00
#define  COLOR_WHITE		0x01
#define  COLOR_ORANGE		0x02
#define  COLOR_RED		0x03
#define  COLOR_GREEN		0x04
#define  COLOR_BLUE		0x05
#define  COLOR_ALL		0x03

#define  LED_BRIGHTNESS_MAX		255

#define  LED_OFF	0x00
#define  LED_ON		0x01

#define  OP_BRIGHTNESS	0x01
#define  OP_COLOR		0x02
#define  OP_ONOFF		0x03
#define  OP_BLINK		0x04
#define  OP_BREATH		0x05

#define  RUN_STATE_OFF			0x0
#define  RUN_STATE_ON			0x1
#define  RUN_STATE_BLINK		0x2
#define  RUN_STATE_BREATH		0x3

#define  STOP_CHECK_TIMES   5
#define  ONESHOT_CHECK_DELAY   3000

#define  LOG_INFO	0x02
#define  LOG_MSG	0x01

#ifndef DEBUG_PRINTK
#define DEBUG_PRINTK(log_level,fmt, ...)
#endif

static unsigned long debug_on;
static int remove_or_shutdown;

static u8 led_color_table[5][3] = {
  {255, 255, 255},  /* white  180 */   
  {220, 40,  00},   /* oringe  255*/
  {255, 00,  00},    /* R  */
  {00,  255, 00},    /* G  */
  {00,  00, 255}    /* B  */
  
};
char * str_op_mode[6] = { "off", "on", "blink", "breath"};
char * str_led_name[] = { "sys", "net", "disk1", "disk2", "disk3", "disk4"};
  
typedef struct led_state {
    int ignore_read_num;
    int need_read;    /* need read from mcu */
    atomic_t brightness;   /* real brightness */
    atomic_t color;
    atomic_t run_state;   // 0-off, 1-on, 2-blink, 3-breath
    int check_times;	/*  times of  trig_oneshots is zero ,then stop delay check work  */
    atomic_t t_blink[2];
    atomic_t t_breath[2];
}led_state_t;

struct mcu_led {
    u8 led_bit;
    u8 color;
    unsigned long delay_on;
    unsigned long delay_off;
    const char *name;
    struct led_classdev cdev;
    struct work_struct	work;
    struct delayed_work	check_work;	/* ata_oneshot_check_work */
    int brightness;  /* indicate on,of */
    int value;		 /* indicate real brightness */
    led_state_t  cur_state;
};

static struct mcu_led ug_leds[LED_COUNT] =  { 
    { .name = "power",   .color=COLOR_NONE, .cur_state={.ignore_read_num=3, .need_read=0, .check_times=0, }},
    { .name = "network_stat", .color=COLOR_NONE, .cur_state={.ignore_read_num=3, .need_read=0, .check_times=0, }},
    { .name = "disk1", 	 .color=COLOR_NONE, .cur_state={.ignore_read_num=3, .need_read=0, .check_times=0, }},
    { .name = "disk2",   .color=COLOR_NONE, .cur_state={.ignore_read_num=3, .need_read=0, .check_times=0, }},

};

static void init_leds_stats(void){
    int i;
    for(i = 0; i < LED_COUNT; i++){
        atomic_set(&ug_leds[i].cur_state.brightness,255);
        atomic_set(&ug_leds[i].cur_state.color,COLOR_NONE);
        if(0 == i){
            atomic_set(&ug_leds[i].cur_state.run_state, LED_ON);
        }
        else{
            atomic_set(&ug_leds[i].cur_state.run_state, LED_OFF);
        }
    }
    pr_info("---%s \n", __func__);
}

static int get_led_addr_by_name(const char *name)
{
    if (!name)
        return -EINVAL;
    if (!strcmp(name, "power"))
        return 0;
    else if (!strcmp(name, "network_stat"))
        return 1;
    else if (!strcmp(name, "disk1"))
        return 2;
    else if (!strcmp(name, "disk2"))
        return 3;
    else if (!strcmp(name, "disk3"))
        return 4;
    else if (!strcmp(name, "disk4"))
        return 5;
    else
        return -EINVAL;
}

static int do_color_set(int addr,int color);

static int led_set_on_off(int addr, int run_state) {	
    if((0 > addr) || (LED_COUNT < addr) ) {
        pr_info("%s addr:%d is invalid\n",__func__, addr);
        return -EINVAL;
    }
    if ((run_state != LED_OFF) && (run_state != LED_ON) ){
        pr_info("%s run_state:%d is invalid\n",__func__, run_state);
        return -EINVAL;
    }
    if(run_state == atomic_read(&ug_leds[addr].cur_state.run_state )) {
        DEBUG_PRINTK(LOG_INFO, "%s run_state:%d is already ,need't to set\n",__func__, run_state);
        return 0;
    }

    if(run_state != atomic_read(&ug_leds[addr].cur_state.run_state)) {
        if(mutex_lock_interruptible(&ug_201x_lock) < 0){
            pr_err("led on off mux lock failed!\n");
            return -EAGAIN;
        }
        ArgbModeSet(addr,run_state,0,0);		/*	TODO */

        mutex_unlock(&ug_201x_lock);
        atomic_set(&ug_leds[addr].cur_state.run_state,run_state);
    }
    else {
        DEBUG_PRINTK(LOG_INFO, "%s line:%d,run_state:%d is already ,need't to set\n",__func__, __LINE__,run_state);
    }
    return 0;
}

static int do_brightness_set(int addr, int brightness) {
    int run_state = -1;
    DEBUG_PRINTK(LOG_MSG,"%s, addr:%d, bri:%d\n", __func__,addr,brightness);
    if(1 == brightness){
        run_state = LED_ON;
    }
    else if( 0 == brightness) {
        run_state = LED_OFF;
    }

    if(run_state != -1) {
        if(run_state != atomic_read(&ug_leds[addr].cur_state.run_state))
            led_set_on_off(addr,run_state);
        return 0;
    }
    		
    if(atomic_read(&ug_leds[addr].cur_state.brightness) != brightness) {
        if(mutex_lock_interruptible(&ug_201x_lock) < 0){
            pr_err("led brightness mux lock failed!\n");
            return -EAGAIN;
        }

        ArgbLightSet(addr,brightness);		/*	TODO */
        mutex_unlock(&ug_201x_lock);
        atomic_set(&ug_leds[addr].cur_state.brightness,brightness);
    }
    else {
        DEBUG_PRINTK(LOG_INFO, "%s brightness:%d is already ,need't to set\n",str_led_name[addr], brightness);
    }
    return 0;
}

static void mcu_work(struct work_struct *work){
    if (remove_or_shutdown) {
        DEBUG_PRINTK(LOG_MSG,"---[%s]remove_or_shutdown:%d, exiting\n", __func__, remove_or_shutdown);
        return;
    }
    struct mcu_led *led = container_of(work, struct mcu_led, work);
    //unsigned char val = (led->color << led->led_bit);
    int brightness = led->value;
    int addr;

    struct led_classdev *cdev = &led->cdev;
    if (cdev == NULL || cdev->name == NULL) {
        DEBUG_PRINTK(LOG_MSG,"---[%s]cdev or cdev->name is NULL, exiting\n", __func__);
        return;
    }
    
    addr = get_led_addr_by_name(cdev->name);
    if (addr < 0 || addr >= LED_COUNT) {
        DEBUG_PRINTK(LOG_MSG,"---[%s]invalid addr:%d, exiting\n", __func__, addr);
        return;
    }
    if (!remove_or_shutdown) {
        do_brightness_set(addr, brightness);
    }
}

static void leds_brightness_set(struct led_classdev *cdev, enum led_brightness brightness){
    if( 1 == remove_or_shutdown) {
        DEBUG_PRINTK(LOG_MSG,"---%s remove_or_shutdown:%d \n", __func__, remove_or_shutdown);
        return ;
    }
    struct mcu_led *led = container_of(cdev, struct mcu_led, cdev);
    if( 0 > brightness || LED_BRIGHTNESS_MAX < brightness) {
        pr_err("---%s brightness:%d is invalid \n", __func__, brightness);
        return ;
    }
    led->value = brightness;
    schedule_work(&led->work);
}

static int led_blink_or_breath(int addr, int op_mode, unsigned long hight, unsigned long low) {
    if((0 > addr) || (LED_COUNT-1 < addr) ) {
        pr_info("%s addr:%d is invalid\n",__func__, addr);
        return -EINVAL;
    }
    if(op_mode == (atomic_read(&ug_leds[addr].cur_state.run_state ) + 2)) {
        if(op_mode == OP_BLINK && hight == atomic_read(&ug_leds[addr].cur_state.t_blink[0]) && low == atomic_read(&ug_leds[addr].cur_state.t_blink[1]) ) {
            DEBUG_PRINTK(LOG_INFO, "%s op_mode:%d is already ,need't to set\n",__func__, op_mode);
            return 0;
        }
        if(op_mode == OP_BREATH && hight == atomic_read(&ug_leds[addr].cur_state.t_breath[0]) && low == atomic_read(&ug_leds[addr].cur_state.t_breath[1])) {
            DEBUG_PRINTK(LOG_INFO, "%s op_mode:%d is already ,need't to set\n",__func__, op_mode);
            return 0;
        }
    }

    if ((op_mode == OP_BLINK) || (op_mode == OP_BREATH) ){
        if(mutex_lock_interruptible(&ug_201x_lock) < 0){
            pr_err("led blink or brea mux lock failed!\n");
            return -EAGAIN;
        }
        DEBUG_PRINTK(LOG_INFO, "%s op_mode:%d hight:%lu, low:%lu \n",__func__, op_mode,hight,low);
        ArgbModeSet(addr, op_mode - 2, hight/50, low/50);		/*	TODO */

        mutex_unlock(&ug_201x_lock);
        {
            atomic_set(&ug_leds[addr].cur_state.run_state, op_mode - 2);
            if (op_mode == OP_BLINK){
                atomic_set(&ug_leds[addr].cur_state.t_blink[0], hight);
                atomic_set(&ug_leds[addr].cur_state.t_blink[1], low);
            }
            if (op_mode ==OP_BREATH){
                atomic_set(&ug_leds[addr].cur_state.t_breath[0], hight);
                atomic_set(&ug_leds[addr].cur_state.t_breath[1], low);
            }
            //ug_leds[addr].cur_state.do_set_blink_time = jiffies_to_msecs(jiffies);
        }
    }
    else {
        DEBUG_PRINTK(LOG_INFO, "%s do nothing, addr:%d, mode:%d is invalid\n",__func__,addr, op_mode);
        return -EINVAL;
    }

	return 0;
}

static int mcu_led_blink_set(struct led_classdev *cdev,unsigned long *delay_on,unsigned long *delay_off){
    u8 addr;
    unsigned long tmp = 0;
    unsigned long blink_on = 0,blink_off = 0;
    int op_mode = OP_BLINK;
    if( 1 == remove_or_shutdown) {
        DEBUG_PRINTK(LOG_MSG,"---[%s]remove_or_shutdown:%d \n", __func__, remove_or_shutdown);
        return 0;
    }

    if (cdev == NULL || cdev->name == NULL) {
        pr_info("---[%s],line:%d cdev or cdev->name is NULL\n", __func__, __LINE__);
        return -EINVAL;
    }
    if (cdev->trigger == NULL || cdev->trigger->name == NULL) {
        pr_info("---[%s],line:%d cdev->trigger or trigger->name is NULL\n", __func__, __LINE__);
        return -EINVAL;
    }
    DEBUG_PRINTK(LOG_INFO, " %s line:%d, name:%s led_data->name[%s]on[%lu]off[%lu]\n", __func__, __LINE__,cdev->name,cdev->trigger->name,*delay_on,*delay_off);

    addr = get_led_addr_by_name(cdev->name);
    if (addr < 0 || addr >= LED_COUNT) {
        pr_info("%s: invalid led name %s\n", __func__, cdev->name);
        return -EINVAL;
    }
    blink_on = *delay_on;
    blink_off = *delay_off;

    if(!strncmp(cdev->trigger->name, "normal",6)){
        if(RUN_STATE_OFF != atomic_read(&ug_leds[addr].cur_state.run_state) && RUN_STATE_ON != atomic_read(&ug_leds[addr].cur_state.run_state)){
            led_set_on_off(addr, LED_OFF);
        }
        return 0;
    }
    else if(!strcmp(cdev->trigger->name,"timer2")) {
        if((0 == blink_on) || (0 == blink_off)) {
            return 0;
        }
        if(blink_off < 100)
            blink_off = 100;
        if(blink_on < 100)
            blink_on = 100;
    }
    else if(!strcmp(cdev->trigger->name,"breath")) {
        if((0 == blink_on) || (0 == blink_off)) {
            return 0;
        }
        op_mode = OP_BREATH;
        if(blink_off < 500)
            blink_off = 500;
        if(blink_on < 1000)
            blink_on = 1000;
    }
    tmp = blink_on + blink_off;

    led_blink_or_breath(addr, op_mode, tmp, blink_on);

    return 0;
}

static int do_color_set(int addr ,int color){
    int color_index = 0;
    int RgbDat = 0;
    color_index =color -1;

    RgbDat = led_color_table[color_index][0] <<16 ;		/* R */
    RgbDat |= (led_color_table[color_index][1] <<8);		/* G */
    RgbDat |= (led_color_table[color_index][2]);		/* B */

    if(atomic_read(&ug_leds[addr].cur_state.color) != color) {
        if(mutex_lock_interruptible(&ug_201x_lock) < 0){
            pr_err("led color mux lock failed!\n");
            return -EAGAIN;
        }

        ArgbRgbSet(addr, RgbDat);		/*	TODO */
        mutex_unlock(&ug_201x_lock);
        atomic_set(&ug_leds[addr].cur_state.color,color);
    }		
    else {
        DEBUG_PRINTK(LOG_INFO, "%s color:%d is already ,need't to set\n", str_led_name[addr],color);
    }
    return 0;
}

static ssize_t color_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t size){
    int ret;    
    unsigned long state;
    int addr;
    if( 1 == remove_or_shutdown) {
        DEBUG_PRINTK(LOG_MSG,"---[%s]remove_or_shutdown:%d \n", __func__, remove_or_shutdown);
        return size;
    }
    struct led_classdev *cdev = dev_get_drvdata(dev);
    if (led_sysfs_is_disabled(cdev)) {
        ret = -EBUSY;
        return ret;
    }

    ret = kstrtoul(buf, 10, &state);
    if (ret)
        return ret;

    if(state > 5){  // origin is 3
        ret = -EINVAL;
        return ret;
    }
    if(state < 1)
        state = 1;
    state &= 0x07;

    if(cdev == NULL ) {
        pr_info("[%s][%d]size[%lu] not match\n", __func__, __LINE__ ,size);
        return size;
    }

    addr = get_led_addr_by_name(cdev->name);
    if (addr < 0 || addr >= LED_COUNT) {
        pr_info("%s: invalid led name %s\n", __func__, cdev->name);
        return -EINVAL;
    }

    do_color_set(addr, state);

    return size;
}
static DEVICE_ATTR(color, 0644, NULL, color_set);

static struct attribute *ugled201_attrs[] = {
	&dev_attr_color.attr,
	NULL
};
ATTRIBUTE_GROUPS(ugled201);

static int init_leds(void ){
    int i;
    u8 addr;
    int run_state,brightness,ret = 0;
    //int index = 0
    if(mutex_lock_interruptible(&ug_201x_lock) < 0){
        pr_err("init led mux lock failed!\n");
        return -EAGAIN;
    }
    for(i = 0;i < LED_COUNT;i++) {
        addr = i;
        brightness = atomic_read(&ug_leds[i].cur_state.brightness);
        ArgbLightSet(addr,brightness);		/*	brightness */

        run_state = atomic_read(&ug_leds[i].cur_state.run_state);
        ArgbModeSet(addr,run_state,0,0);		/*	on-off */

        //atomic_set(&ug_leds[addr].cur_state.run_state,run_state);
        //usleep_range(200,1500);
        DEBUG_PRINTK(LOG_MSG," ---%s %d set on-off  ret:%d\n", __func__, i,ret);
        //do_color_set(addr, COLOR_WHITE);
    }	
    mutex_unlock(&ug_201x_lock);
    return 0;
}

int ug_201_leds_init(struct platform_device * pdev){
    int i;
    debug_on = 0;
    remove_or_shutdown = 0;

    for(i = 0;i < LED_COUNT;i++) {
        ug_leds[i].cdev.name =ug_leds[i].name;
        ug_leds[i].cdev.brightness_set = leds_brightness_set;
        ug_leds[i].cdev.blink_set = mcu_led_blink_set;
        ug_leds[i].cdev.groups = ugled201_groups;
        ug_leds[i].cdev.max_brightness = LED_BRIGHTNESS_MAX;
        ug_leds[i].color = COLOR_NONE;
        ug_leds[i].brightness = 0;

        INIT_WORK(&ug_leds[i].work, mcu_work);
        led_classdev_register(&pdev->dev, &ug_leds[i].cdev);
    }
    init_leds_stats();
    init_leds();

    return 0;
}

void ug_201_leds_remove(void){
    int i;
    remove_or_shutdown = 1; 
    DEBUG_PRINTK(LOG_MSG,"[%s][%d]\n", __func__, __LINE__);

    for(i = 0;i < LED_COUNT;i++) {
        cancel_work_sync(&ug_leds[i].work);
    }
    for(i = 0;i < LED_COUNT;i++) {
        led_classdev_unregister(&ug_leds[i].cdev);
    }
    msleep(80);
    
    return ;
}

static void ug_201_leds_shutdown(void){
    int i;
    remove_or_shutdown = 1;
    DEBUG_PRINTK(LOG_MSG,"---[%s][%d], sleep 3s \n", __func__, __LINE__);
    msleep(3300);
    DEBUG_PRINTK(LOG_MSG,"---[%s][%d]\n", __func__, __LINE__);

    for(i = 0;i < LED_COUNT;i++) {
        cancel_work_sync(&ug_leds[i].work);
    }
    for(i = 0;i < LED_COUNT;i++) {
        led_classdev_unregister(&ug_leds[i].cdev);
    }
    pr_info("---[%s][%d] end\n", __func__, __LINE__);
    //msleep(80);
    
    return ;
}
