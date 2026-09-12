// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author:Jason.Li
 * date:2026/1/15
 * Description: For 2800gt
 *		control fan,power-status,temperature,argb-led,watchdog Driver.
 * Chip: CSCS201X
 * 
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <linux/dmi.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
//#include <misc/ugreen_product.h>

#undef pr_fmt
#define pr_fmt(fmt) "201X: " fmt

#define NO_DEV_ID	0xffff
#define CS201X_ID	0x2011

#define WATCHDOG_NAME		"201x WDT"
/* Defaults for Module Parameter */
#define DEFAULT_TIMEOUT		1800
#define DEFAULT_TESTMODE	0
#define DEFAULT_NOWAYOUT	WATCHDOG_NOWAYOUT
#define  max_units	65535
/* IO Ports */
#define REG		0x2e
#define VAL		0x2f

/* Configuration Registers and Functions */
#define LDNREG		0x07
#define CHIPID		0x20
#define CHIPREV		0x22

#define PME     0x04    /* The device with the fan registers in it */
#define H2EC_LDN     0x0A
// fan2 is sys_fan

//#define FAN2_PWM_CTRL_REG	0x16
//#define FAN2_PWM_DATA_REG	0x6B

#define FAN2_PWM_CTRL_REG	0x17
#define FAN2_PWM_DATA_REG	0x73

//#define FAN3_PWM_CTRL_REG	0x17  //fan3
//#define FAN3_PWM_DATA_REG	0x73 // fan3

#define FAN2_SOFTWARE_MODE	0
#define FAN2_AUTO_MODE		1

#define BUTN    0 /* 开机键启动*/ 
#define AUTO    1 /* 上电自动启动 */
#define LAST    2 /* 最后状态动态*/

#define MAIN_DIR	"it86"

extern void ug_201_leds_remove(void);
extern int ug_201_leds_init(struct platform_device *pdev);

struct proc_dir_entry *nas_dir = NULL;
struct mutex ug_201x_lock;

static ushort ec_addr_port;
static ushort ec_data_port;

static void * amd_poafp_addr = NULL;

#define IMA_HOST_CTS    	0x00
#define IMA_CTRL        	0x01
#define IMA_ADDR        	0x04
#define IMA_DATA        	0x08
#define IMA_SEM         	0x0C
 
#define FAN0_EN 			0x200053F0
#define FAN0_DUTY    	 	0x200053F4
#define FAN0_SPEED_H     	0x200053F8
#define FAN0_SPEED_L     	0x200053F9

#define FAN1_EN 			0x200053F1
#define FAN1_DUTY    	 	0x200053F5
#define FAN1_SPEED_H     	0x200053FA
#define FAN1_SPEED_L     	0x200053FB

#define ADC_CH2_CFG				0x20005370
#define ADC_CH4_CFG				0x20005371

#define NTC2_TEMPERATURE  	0x200053E2
#define NTC4_TEMPERATURE  	0x200053E4

#define ARGB_FLOW_CONTROL  	0x20005440
#define SIO_FW_VER  		0x200054FC
#define PWRSW_CFG  			0x2000548C
#define	WDT_CFG1			0x20005480
#define	WDT_CFG2			0x20005481
#define	WDT_TIMEOUT_H		0x20005482
#define	WDT_TIMEOUT_L		0x20005483
#define	WDT_COUNTER_H		0x20005484
#define	WDT_COUNTER_L		0x20005485
#define	WDT_FEED			0x20005488

#define	ARGBx_DATA01_B		0x20005444
#define	ARGBx_DATA01_R		0x20005445
#define	ARGBx_DATA01_G		0x20005446
#define	ARGB_LIGHT			0x20005404	/*ARGB_DX_V */

#define	ARGB0_MOD			0x20005400
#define	ARGB0_PERIOD		0x20005408	/*ARGB0_DX_CR_NUM */
#define	ARGB0_DUTY			0x2000540C

#define	POAPF_CFG			0x20005491	/*  Power On after Power fail:cfg */
#define	POAPF_UPD			0x20005493	/*  Power On after Power fail:upd */

#define	AMD_POAPF_CFG		0xFED8035B

enum
{
    BIOS_TURNON_OFF = 0,
    BIOS_TURNON_ON,
    RAID_TURNON_LAST_STATE,
};
static void ug_msleep(int t){
#ifndef CONFIG_UG_FEAT_MSLEEP
	return;
#else
	msleep(t);
	return;
#endif
}

static int ug_mutex_trylock(int cnt){
	int i; 
	if( 0 == cnt)
		cnt = 3;
	for(i = 0; i < cnt; i++){
		if(mutex_trylock(&ug_201x_lock))
			return 0;
		if (msleep_interruptible(5))
			return -EINTR; 
	}
	return -1;
}

static inline int superio_inw(int reg)
{
	int val;

	outb(reg++, REG);
	val = inb(VAL) << 8;
	outb(reg, REG);
	val |= inb(VAL);
	return val;
}

static inline int superio_inb(int reg)
{
	outb(reg, REG);
	return inb(VAL);
}

static inline void superio_exit(void)
{
#if 0
	pr_info("ignore superio_exit\n");
#else
	outb(0x87, REG);
	outb(0x69, REG);
	outb(0xA5, REG);
#endif 
	release_region(REG, 2);
	ug_msleep(1);
}

static inline int superio_enter(void)
{
	/*
	 * Try to reserve REG and REG + 1 for exclusive access.
	 */
	if (!request_muxed_region(REG, 2, KBUILD_MODNAME)){
		pr_err("superio_enter request_muxed_region failed\n");
		return -EBUSY;
	}
#if 0
	pr_info("ignore superio_enter\n");
#else
	outb(0xA5, REG);
	ug_msleep(1);
	outb(0x69, REG);
	ug_msleep(1);
	outb(0x87, REG);
	ug_msleep(1);
#endif
	return 0;
}

static inline void superio_select(int ldn)
{
	outb(LDNREG, REG);
	outb(ldn, VAL);
	ug_msleep(1);
}

/* Write a byte to the Super IO configuration register */
static void Chipsea_Host2ec_Outb(int ioreg, int reg, int val)
{
	//pr_info("---%s:ioreg:%#x, reg:%#x, val:%#x \n", __func__,ioreg,reg,val);
    // step1  AUTO_INC disable, and one byte
    outb(0x00,(u16)(ioreg + IMA_CTRL));

    // step2 write the argb  num  address to IMA_ADDR 3~0
    outb((u8)((reg >> 24) & 0xFF), (u16)(ioreg + IMA_ADDR + 3));
    outb((u8)((reg >> 16) & 0xFF), (u16)(ioreg + IMA_ADDR + 2));
    outb((u8)((reg >> 8) & 0xFF),  (u16)(ioreg + IMA_ADDR + 1));
    outb((u8)(reg & 0xFF),	(u16)(ioreg + IMA_ADDR));

    // write the argb NUM value
    outb((u8)val,	(u16)(ioreg + IMA_DATA));
    ug_msleep(1);
    return;
}

/* Read a byte to the Super IO configuration register */
static u8 Chipsea_Host2ec_Inb(int ioreg, int reg)
{
    //step1  AUTO_INC disable, and one byte
    outb(0x00,	(u16)(ioreg + IMA_CTRL));

    //step2 write the argb mod  address to IMA_ADDR 3~0
    outb((u8)((reg >> 24) & 0xFF), (u16)(ioreg + IMA_ADDR + 3));
    outb((u8)((reg >> 16) & 0xFF), (u16)(ioreg + IMA_ADDR + 2));
    outb((u8)((reg >> 8) & 0xFF),  (u16)(ioreg + IMA_ADDR + 1));
    outb((u8)(reg & 0xFF),			(u16)(ioreg + IMA_ADDR) );
    ug_msleep(1);
    //read the argb mode value
    return inb((u16)(ioreg + IMA_DATA));

}

 /* Host2EC（LDN=0Ah, Index 30h=01） */
static inline void InitEc(void )
{
    superio_select(H2EC_LDN);

    ug_msleep(1);
    /* get ec base addr */
    ushort ec_base = (ushort)superio_inw(0x60);
    pr_info("ec_base addr:%#x",ec_base);
    ec_addr_port = (ushort)(ec_base) ;
    ec_data_port = (ushort)(ec_base + 0x08);
    pr_info("ec_addr_port:%#x,ec_data_port:%#x",ec_addr_port,ec_data_port);
    ug_msleep(1);

    /* enable ec */
    outb(0x30, REG);
    outb(0x01, VAL);
    Chipsea_Host2ec_Outb(ec_addr_port, FAN0_EN, 1);
    Chipsea_Host2ec_Outb(ec_addr_port, FAN1_EN, 1);
    //msleep(1);
    Chipsea_Host2ec_Outb(ec_addr_port, ADC_CH2_CFG, 1); 	/*	set adc2: temperature input */
    //msleep(1);
    Chipsea_Host2ec_Outb(ec_addr_port, ADC_CH4_CFG, 1); 	/*	set adc4: temperature input */
    //msleep(1);
}

static inline int GetNTCTemperature(int TemperatureId)
{
    int  val = 0;

    //Chipsea_Host2ec_Outb(ec_addr_port, ADC_CH2_CFG, 1);
    //Chipsea_Host2ec_Outb(ec_addr_port, ADC_CH4_CFG, 1);
	//pr_info("ec_addr_port:%#x,temp_id:%d\n",ec_addr_port,TemperatureId);
    if(TemperatureId == 0) {
        val = Chipsea_Host2ec_Inb(ec_addr_port,NTC2_TEMPERATURE);//200053E2
    }
    else if(TemperatureId == 1) {
        val = Chipsea_Host2ec_Inb(ec_addr_port,NTC4_TEMPERATURE);//200053E4
    }

    return val;
}

static inline int GetFanRpm(int FanId)
{
    int fan_speed = 0;
    u32 lval = 0;
    u32 hval = 0;
    if(FanId <= 4)
    {
        lval = Chipsea_Host2ec_Inb(ec_addr_port, FAN0_SPEED_L + (2 * FanId));//200053f8-200053fb
		//pr_info("---%s,low:%#x\n", __func__,lval);
        ug_msleep(1);
		hval = Chipsea_Host2ec_Inb(ec_addr_port, FAN0_SPEED_H + (2 * FanId));
		//pr_info("---%s,high:%#x\n", __func__,hval);
        fan_speed = (hval << 8) | lval;
    }
	
    return fan_speed;
}

static inline int SetFanDuty(int FanId, int Duty)
{
    int rc = 0;
    mutex_lock(&ug_201x_lock);
	//pr_info("---%s,id:%d,duty:%d \n", __func__,FanId,Duty);

    if(FanId <= 1){
        Chipsea_Host2ec_Outb(ec_addr_port, FAN0_DUTY + FanId, Duty*100/255);//200053f4-200053f5
    }
    else {
        rc = -1;
        pr_info("invalid fanid:%d\n",FanId);
    }

    mutex_unlock(&ug_201x_lock);

    return rc;
}

/* Read back the current PWM duty cycle, mirroring SetFanDuty().
 * The register stores a 0-100 percentage; callers expect the usual
 * hwmon 0-255 pwm scale, so the conversion happens in the hwmon read
 * callback, not here (this keeps the function symmetrical with the
 * register layout, like GetFanRpm()). */
static inline int GetFanDuty(int FanId)
{
    int val = 0;

    if (FanId <= 1) {
        val = Chipsea_Host2ec_Inb(ec_addr_port, FAN0_DUTY + FanId);//200053f4-200053f5
    }

    return val;
}
/* pin 0 is krst# */
static void Wdt_init(int pin, int timeout)
{
     //DISABLE WDT
    if(timeout == 0){
        pr_info("%s:err timeout;%d\n", __func__,timeout);
        return ;
    }
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG2, 0);
    if(pin == 0)
    {
        Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG1, 0x33);  //set pin to krst#, and set timeout unit to second
    }
    else
    {
        //TBD hardwre.
        Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG1, 0x32);
    }
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_TIMEOUT_H, ((timeout & 0xFF00)>>8));
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_TIMEOUT_L, (timeout & 0xFF));
    //msleep(500);
    return ;
}

static void Wdt_cfg_timeout(int timeout)
{

    Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG2, 0);
    //msleep(100);
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_TIMEOUT_H, ((timeout & 0xFF00)>>8));
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_TIMEOUT_L, (timeout & 0xFF));
    msleep(1000);
    pr_info("%s:%d\n", __func__,timeout);
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG2, 1);
    return ;
}

static int WdtStart(int EnSta)
{
    if(EnSta > 1){
        pr_info("%s:err EnSta;%d\n", __func__,EnSta);
        return -1;
    }
    Chipsea_Host2ec_Outb(ec_addr_port, WDT_CFG2, EnSta);
    return 0;
}
/* ***************************ARGB********************************* */
/* ArgbId need 0-3. */
void ArgbRgbSet(int ArgbId, int RgbDat)
{
    if(ArgbId < 4)
    {
        Chipsea_Host2ec_Outb(ec_addr_port, ARGBx_DATA01_B + (ArgbId << 2), (RgbDat & 0xFFU));//B
        Chipsea_Host2ec_Outb(ec_addr_port, ARGBx_DATA01_R + (ArgbId << 2), (RgbDat & 0xFF0000U) >> 16);//R
        Chipsea_Host2ec_Outb(ec_addr_port, ARGBx_DATA01_G + (ArgbId << 2), (RgbDat & 0xFF00U) >> 8);//G
    }
    return ;
}

void ArgbLightSet(int ArgbId,int Light)
{
    if(ArgbId < 4 && Light <= 255)
    {
        pr_info("%s, id:%d, light:%d \n", __func__, ArgbId,Light);
        Chipsea_Host2ec_Outb(ec_addr_port, ARGB_LIGHT + ArgbId, Light);
    }
    return ;
}

void ArgbModeSet(int ArgbId, int Mode, int period, int duty)
{
	if(ArgbId < 4)
	{
		Chipsea_Host2ec_Outb(ec_addr_port, ARGB0_MOD + ArgbId, Mode);
		Chipsea_Host2ec_Outb(ec_addr_port, ARGB0_PERIOD + ArgbId, period);
		Chipsea_Host2ec_Outb(ec_addr_port, ARGB0_DUTY + ArgbId, duty);
	}
    return ;
}

/*		AUTO POWER ON		*/
/* 0 always off   1 always on   2 abnormal shut down will power on, normal shut down will not power on. */
static int AutoPowerOnGet(void)
{
    return  __raw_readl(amd_poafp_addr) & 0x3;
#if 0
    return Chipsea_Host2ec_Inb(ec_addr_port, 0x0801B810);
#endif
}

static int AutoPowerOnSet(int mode)
{
    u32 val;
    if(mode > 3){
        return -1;
    }
    val = __raw_readl(amd_poafp_addr);
    val &=   ~0x3;
    val |= mode;
    __raw_writel(val,amd_poafp_addr);
#if 0
    Chipsea_Host2ec_Outb(ec_addr_port, POAPF_CFG, mode);
    if(AutoPowerOnGet() != mode)
    {
        Chipsea_Host2ec_Outb(ec_addr_port, POAPF_UPD, 1);
    }
#endif
    return 0;
}
		
		
static ssize_t fan_read(struct file *file, char __user *usr_buf, size_t size, loff_t *ppos)
{
    char tmpbuf[128];
    int rc = 0;
    unsigned int cnt=0;
    unsigned long speed;

    if (*ppos > 0)
        return 0;
    if (mutex_lock_interruptible(&ug_201x_lock) < 0){
        return -EAGAIN;
    }

    speed =  GetFanRpm(0);

    cnt = snprintf(tmpbuf, sizeof(tmpbuf), "cpufan speed:%lu\n",speed);
    speed =  GetFanRpm(1);
    cnt += snprintf(tmpbuf + cnt, sizeof(tmpbuf) - cnt, "sysfan1 speed:%lu\n",speed);

    if(cnt == 0){
    	cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%d", 0);
    }

    rc = simple_read_from_buffer(usr_buf, size, ppos, tmpbuf, cnt);
	
    mutex_unlock(&ug_201x_lock);
    return rc;
}

static ssize_t fan_write(struct file *file,const char __user * buffer,size_t count, loff_t * ppos)
{
	int result = 0;
	char buf[12] = { '\0' };
	char *p;
	unsigned long pwm;

	if (count > sizeof(buf) - 1){
		return -EINVAL;
	}
	if (copy_from_user(buf, buffer, count)) {
		result = -EFAULT;
		goto end;
	}
	buf[count] = '\0';
	if(buf[count -1] == 0x0a) buf[count - 1] = '\0';
	
	if((!strncmp(buf, "on",3)) || (!strncmp(buf, "ON",3))){
		SetFanDuty(1, 127);
	}else if((!strncmp(buf, "off",4)) || (!strncmp(buf, "OFF",4))){
		SetFanDuty(1, 0);
	}else if((!strncmp(buf, "set ",4)) || (!strncmp(buf, "SET ",4))){
		p = buf+4;
		while((*p < 0x21) && (*p != '\0'))p++;	
		if(*p != '\0'){
		     if(kstrtoul(p, 10, &pwm)){
		        result = -EINVAL;
				goto end;
		      }
		     if((pwm > 0) && (pwm < 256)){
		        SetFanDuty(1, pwm);
		     }else{
		        result = -EINVAL;
		    }
		}else{
		     result = -EINVAL;
		}
	}
	/*  cpu fan */
	else if((!strncmp(buf, "con",3)) || (!strncmp(buf, "CON",3))){
		SetFanDuty(0, 127);
	}else if((!strncmp(buf, "coff",4)) || (!strncmp(buf, "COFF",4))){
		SetFanDuty(0, 0);
	}else if((!strncmp(buf, "cpu ",4)) || (!strncmp(buf, "CPU ",4))){
		p = buf+4;
		while((*p < 0x21) && (*p != '\0'))p++;	
		if(*p != '\0'){
		     if(kstrtoul(p, 10, &pwm)){
		        result = -EINVAL;
			goto end;
		      }
		     if((pwm > 0) && (pwm < 256)){
		        SetFanDuty(0, pwm);
		     }else{
		        result = -EINVAL;
		    }
		}else{
		     result = -EINVAL;
		}
	}
	else{
		result = -EINVAL;
	}
end:
	if(result){
		return result;
	}
	return count;
}

static ssize_t startup_read(struct file *file, char __user *usr_buf, size_t size, loff_t *ppos)
{
    char tmpbuf[128];
    u8	val = 0;
    int rc = 0;
    int cnt;

    if (*ppos > 0)
        return 0;
    mutex_lock(&ug_201x_lock);

    val = AutoPowerOnGet();

    switch(val)  /* <<IT8613_L_V0.9xxx.pdf>> page:33 */
    {
        //case 0x00:     //on
        case 0x01:
            cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%s\n","power on");
            break;
        case 0x00:	  // off
            cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%s\n","power off");
            break;
        case 0x02:    // last status
        case 0x03:
            cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%s\n","last status");
            break;
        default:
            cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%s\n","unkown status");
            break;
    }

    rc =  simple_read_from_buffer(usr_buf, size, ppos, tmpbuf, cnt);

    mutex_unlock(&ug_201x_lock);
    return rc;
}
static ssize_t startup_write(struct file *file,const char __user * buffer,size_t count, loff_t * ppos)
{
    int result = 0;
    char buf[12] = { '\0' };

    if (count > sizeof(buf) - 1){
        return -EINVAL;
    }
    if (copy_from_user(buf, buffer, count)) {
        result = -EFAULT;
        goto end;
    }
    mutex_lock(&ug_201x_lock);
    buf[count] = '\0';
    if(buf[count -1] == 0x0a) buf[count - 1] = '\0';
    if((!strncmp(buf, "on",3)) || (!strncmp(buf, "ON",3))){
        AutoPowerOnSet(1);
    }else if((!strncmp(buf, "off",4)) || (!strncmp(buf, "OFF",4))){
        AutoPowerOnSet(0);
    }else if((!strncmp(buf, "last",5)) || (!strncmp(buf, "LAST",5))){
        //AutoPowerOnSet(2);
        AutoPowerOnSet(3);
    }else{
        result = -EINVAL;
    }
	mutex_unlock(&ug_201x_lock);
end:
    if(result){
        return result;
    }
    return count;
}

static ssize_t temp_read(struct file *file, char __user *usr_buf, size_t size, loff_t *ppos)
{
    char tmpbuf[128];
    int	val = 0;
    int rc = 0;
    int cnt;

    if (*ppos > 0)
        return 0;
    mutex_lock(&ug_201x_lock);

    val = GetNTCTemperature(0);
    cnt = snprintf(tmpbuf, sizeof(tmpbuf), "cpu_temp:%d\n",val);
    val = GetNTCTemperature(1);
    cnt += snprintf(tmpbuf + cnt, sizeof(tmpbuf) - cnt, "board_temp:%d\n",val);

    if(cnt == 0){
    	cnt = snprintf(tmpbuf, sizeof(tmpbuf), "%d", 0);
    }

    rc =  simple_read_from_buffer(usr_buf, size, ppos, tmpbuf, cnt);

    mutex_unlock(&ug_201x_lock);
    return rc;
}

static int null_proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, NULL, pde_data(inode));
}

static const struct proc_ops fan_proc_fops = {
    .proc_open           = null_proc_open,
    .proc_write		= fan_write,
    .proc_read		= fan_read,
    .proc_release        = seq_release,
};

static const struct proc_ops startup_proc_fops = {
    .proc_open           = null_proc_open,
    .proc_write		= startup_write,
    .proc_read		= startup_read,
    .proc_release        = seq_release,
};

static const struct proc_ops temp_proc_fops = {
    .proc_open           = null_proc_open,
    .proc_read		= temp_read,
    .proc_release        = seq_release,
};

static int wdt_start(struct watchdog_device *wdd)
{
	if(mutex_lock_interruptible(&ug_201x_lock) < 0){
		pr_err("wdt start mux lock failed!\n");
		return -EAGAIN;
	}
	pr_info("---%s \n", __func__);
	WdtStart(1);

	mutex_unlock(&ug_201x_lock);
	return 0;
}

static int wdt_stop(struct watchdog_device *wdd)
{
	if(mutex_lock_interruptible(&ug_201x_lock) < 0){
		pr_err("wdt stop mux lock failed!\n");
		return -EAGAIN;
	}
	pr_info("---%s \n", __func__);
	WdtStart(0);

	mutex_unlock(&ug_201x_lock);
	return 0;
}

static int wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{

	if (t > max_units) {
		t += 59;
		t -= t % 60;
	}

	wdd->timeout = t;

	if(mutex_lock_interruptible(&ug_201x_lock) < 0){
		pr_err("wdt set time out mux lock failed!\n");
		return -EAGAIN;
	}
	pr_info("%s:%d \n", __func__,t);
	Wdt_cfg_timeout(t);

	mutex_unlock(&ug_201x_lock);
	return 0;
}

static const struct watchdog_info ident = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.firmware_version = 1,
	.identity = WATCHDOG_NAME,
};

static const struct watchdog_ops wdt_ops = {
	.owner = THIS_MODULE,
	.start = wdt_start,
	.stop = wdt_stop,
	.set_timeout = wdt_set_timeout,
		
};

static struct watchdog_device wdt_dev = {
	.info = &ident,
	.ops = &wdt_ops,
	.min_timeout = 30,
};

/* ---------------------------------------------------------------------
 * hwmon interface
 *
 * Exposes the same CPU/board NTC temperatures and CPU/system fan RPM
 * values as /proc/it86/{temp,fan}, but through the standard Linux
 * hwmon framework so that lm-sensors ("sensors" command) and any tool
 * built on top of it (Netdata, Brisa, node_exporter, etc.) can read
 * them from /sys/class/hwmon/hwmonX/ without knowing anything about
 * this driver.
 * --------------------------------------------------------------------- */

static const char * const ug201x_temp_label[] = {
	"cpu_temp",
	"board_temp",
};

static const char * const ug201x_fan_label[] = {
	"cpufan",
	"sysfan1",
};

static umode_t ug201x_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel >= ARRAY_SIZE(ug201x_temp_label))
			return 0;
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		return 0;
	case hwmon_fan:
		if (channel >= ARRAY_SIZE(ug201x_fan_label))
			return 0;
		if (attr == hwmon_fan_input || attr == hwmon_fan_label)
			return 0444;
		return 0;
	case hwmon_pwm:
		/* channel 0 = cpufan, channel 1 = sysfan1, matching the
		 * fan/temp channel numbering above so tools like Brisa
		 * pair pwmX with fanX automatically. */
		if (channel > 1)
			return 0;
		if (attr == hwmon_pwm_input)
			return 0644;
		return 0;
	default:
		return 0;
	}
}

static int ug201x_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	int rc = 0;

	if (mutex_lock_interruptible(&ug_201x_lock))
		return -EINTR;

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input) {
			rc = -EOPNOTSUPP;
			break;
		}
		/* GetNTCTemperature() returns a raw value in degrees C,
		 * hwmon wants milli-degrees C. */
		*val = (long)GetNTCTemperature(channel) * 1000;
		break;
	case hwmon_fan:
		if (attr != hwmon_fan_input) {
			rc = -EOPNOTSUPP;
			break;
		}
		/* GetFanRpm() already returns RPM, which is what hwmon
		 * expects for fanX_input. */
		*val = (long)GetFanRpm(channel);
		break;
	case hwmon_pwm:
		if (attr != hwmon_pwm_input) {
			rc = -EOPNOTSUPP;
			break;
		}
		/* GetFanDuty() returns a 0-100 percentage; hwmon's pwmX
		 * convention is 0-255. */
		*val = ((long)GetFanDuty(channel) * 255) / 100;
		break;
	default:
		rc = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&ug_201x_lock);
	return rc;
}

static int ug201x_hwmon_write(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EOPNOTSUPP;
	if (channel > 1)
		return -EINVAL;
	if (val < 0 || val > 255)
		return -EINVAL;

	/* SetFanDuty() already takes/releases ug_201x_lock itself and
	 * already expects a 0-255 value, so it is called directly here
	 * without an extra lock (that would deadlock) or extra scaling. */
	return SetFanDuty((int)channel, (int)val);
}

static int ug201x_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_label)
			return -EOPNOTSUPP;
		*str = ug201x_temp_label[channel];
		return 0;
	case hwmon_fan:
		if (attr != hwmon_fan_label)
			return -EOPNOTSUPP;
		*str = ug201x_fan_label[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops ug201x_hwmon_ops = {
	.is_visible	= ug201x_hwmon_is_visible,
	.read		= ug201x_hwmon_read,
	.write		= ug201x_hwmon_write,
	.read_string	= ug201x_hwmon_read_string,
};

static const struct hwmon_channel_info *ug201x_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			    HWMON_T_INPUT | HWMON_T_LABEL,
			    HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			    HWMON_F_INPUT | HWMON_F_LABEL,
			    HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			    HWMON_PWM_INPUT,
			    HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info ug201x_hwmon_chip_info = {
	.ops	= &ug201x_hwmon_ops,
	.info	= ug201x_hwmon_info,
};

static int procfs_create(void)
{
	struct proc_dir_entry *entry = NULL;
	int ret = 0;

	/* create /proc/nas/ */
	nas_dir = proc_mkdir(MAIN_DIR, NULL);
	if (!nas_dir){
		return -ENODEV;
	}

	entry = proc_create_data("fan", S_IWUGO, nas_dir, &fan_proc_fops, NULL);
	if (!entry) {
		ret = -ENODEV;
		goto remove_dev_dir;
	}

	/* create /proc/nas/startup */
	entry = proc_create_data("startup", S_IRWXUGO, nas_dir, &startup_proc_fops, NULL);
	if (!entry) {
		ret = -ENODEV;
		goto remove_dev_dir;
	}
	
	entry = proc_create_data("temp", S_IWUGO, nas_dir, &temp_proc_fops, NULL);
	if (!entry) {
		ret = -ENODEV;
		goto remove_dev_dir;
	}

done:
	return ret;

remove_dev_dir:
	remove_proc_entry(MAIN_DIR, NULL);
	nas_dir = NULL;
	goto done;
}

static int ug_cs201x_probe(struct platform_device *pdev)
{
	u16 chip_type;
	u16 chip_rev;
	int rc = 0;

	pr_info("---%s start\n", __func__);
	rc = superio_enter();
	if (rc){
		pr_err("---%s,superio_enter failed!\n", __func__);
		return rc;
	}

	chip_type = superio_inw(CHIPID);
	chip_rev  = superio_inw(CHIPREV);

	if(chip_type != CS201X_ID){
		pr_err("Unknown Chip found, Chip %04x Revision %x\n",chip_type, chip_rev);
		superio_exit();
		return -ENODEV;
	}
	pr_info("Found chip 201x rev: %x,%x \n",chip_type, chip_rev);
	InitEc();
	superio_exit();
	Chipsea_Host2ec_Outb(ec_addr_port, PWRSW_CFG, 0);		/* disable PWRSW BYPASS */	
	Chipsea_Host2ec_Outb(ec_addr_port, ARGB_FLOW_CONTROL, 1);		/* disable argb led flow */	
	Wdt_init(0, DEFAULT_TIMEOUT);
	wdt_dev.timeout = DEFAULT_TIMEOUT;
	wdt_dev.max_timeout = max_units * 60;

	amd_poafp_addr = ioremap(AMD_POAPF_CFG, 4);
	if(NULL == amd_poafp_addr) {
		pr_err("map amd poafp failed !\n");
		return -ENODEV;
	}
	mutex_init(&ug_201x_lock);
#if 1
	watchdog_stop_on_reboot(&wdt_dev);
	rc = watchdog_register_device(&wdt_dev);
	if (rc) {
		pr_err("Cannot register watchdog device (err=%d)\n", rc);
		return rc;
	}
#endif 
	procfs_create();

	{
		struct device *hwmon_dev;

		hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
						"ug201x", NULL,
						&ug201x_hwmon_chip_info, NULL);
		if (IS_ERR(hwmon_dev)) {
			/* Not fatal: keep the /proc interface working even
			 * if hwmon registration fails for some reason. */
			pr_err("failed to register hwmon device (err=%ld)\n",
			       PTR_ERR(hwmon_dev));
		} else {
			pr_info("hwmon device registered (sensors will show it as \"ug201x\")\n");
		}
	}

	ug_201_leds_init(pdev);

	return 0;
}

static void ug_cs201x_remove(struct platform_device * pdev){
	Chipsea_Host2ec_Outb(ec_addr_port, ARGB_FLOW_CONTROL, 0);		/* disable argb led flow */
	wdt_stop(NULL);
	watchdog_unregister_device(&wdt_dev);
	remove_proc_entry("fan", nas_dir);
	remove_proc_entry("startup", nas_dir);
	remove_proc_entry("temp", nas_dir);
	remove_proc_entry("version", nas_dir);
	remove_proc_entry(MAIN_DIR, NULL);
	ug_201_leds_remove();
	mutex_destroy(&ug_201x_lock);
	iounmap(amd_poafp_addr);
	//superio_exit();
	pr_info("bios_exit\n");
	
	return;
}

static void ug_cs201x_device_release(struct device *dev){
	pr_info("---%s \n", __func__);
	return;
}

static struct platform_driver ug_cs201x_driver = {
	.probe	= ug_cs201x_probe,
	.driver	= {
		.name = "ug-sio201",
	},
	.remove = ug_cs201x_remove,
};

static struct platform_device ug_cs201x_device = {
    .name   = "ug-sio201",
    .dev = {
          .release = ug_cs201x_device_release,
   }

};

static int __init cs201x_init(void)
{
    int ret;
    ret = platform_driver_register(&ug_cs201x_driver);

    if (!ret) {
        ret = platform_device_register(&ug_cs201x_device);
        if (ret)
            platform_driver_unregister(&ug_cs201x_driver);
    }
    return ret;

}
module_init(cs201x_init);

static void __exit cs201x_exit(void)
{
    platform_driver_unregister(&ug_cs201x_driver);
    platform_device_unregister(&ug_cs201x_device);
}

module_exit(cs201x_exit);
MODULE_AUTHOR("lijiang@ugreen.com");
MODULE_DESCRIPTION("ugreen cs201x driver");
MODULE_LICENSE("GPL v2");
