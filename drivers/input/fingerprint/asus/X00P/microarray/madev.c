/* Copyright (C) MicroArray
 * MicroArray Fprint Driver Code for REE enviroment
 * madev.c
 * Date: 2017-3-15
 * Version: v4.0.06
 * Author: guq
 * Contact: guq@microarray.com.cn
 */
#include "madev.h"



//spdev use for recording the data for other use
static unsigned int irq, ret;
static unsigned int ma_drv_reg;
static unsigned int ma_speed;
static unsigned int is_screen_on;
static struct notifier_block notifier;
static unsigned int int_pin_state;
static unsigned int compatible;
static unsigned int screen_flag;
static unsigned int free_check;
static DECLARE_WAIT_QUEUE_HEAD(screenwaitq);
static DECLARE_WAIT_QUEUE_HEAD(gWaitq);
static DECLARE_WAIT_QUEUE_HEAD(U1_Waitq);
static DECLARE_WAIT_QUEUE_HEAD(U2_Waitq);
struct wakeup_source gProcessWakeLock;
struct wakeup_source gIntWakeLock;
struct work_struct gWork;
struct workqueue_struct *gWorkq;
//
static LIST_HEAD(dev_list);
static DEFINE_MUTEX(dev_lock);
static DEFINE_MUTEX(drv_lock);
static DEFINE_MUTEX(ioctl_lock);
#ifdef COMPATIBLE_VERSION3
static DECLARE_WAIT_QUEUE_HEAD(drv_waitq);
#endif

static struct fprint_dev *sdev = NULL;
static struct fprint_spi *smas = NULL;

static u8 stxb[FBUF];
static u8 srxb[FBUF];



static void mas_work(struct work_struct *pws) {
	__pm_wakeup_event(&gIntWakeLock, 1*HZ);
    smas->f_irq = 1;
    wake_up(&gWaitq);
#ifdef COMPATIBLE_VERSION3
	wake_up(&drv_waitq);
#endif
}

static irqreturn_t mas_interrupt(int irq, void *dev_id) {
#ifdef DOUBLE_EDGE_IRQ
	if(mas_get_interrupt_gpio(0)==1){
		//TODO IRQF_TRIGGER_RISING
	}else{
		//TODO IRQF_TRIGGER_FALLING
	}
#else
    queue_work(gWorkq, &gWork);
#endif
	return IRQ_HANDLED;
}


/*---------------------------------- fops ------------------------------------*/

/* 读写数据
 * @buf 数据
 * @len 长度
 * @返回值：0成功，否则失败
 */
int mas_sync(u8 *txb, u8 *rxb, int len) {
    int ret = 0;
#ifdef REE
	mutex_lock(&dev_lock);
    mas_select_transfer(smas->spi, len);
    smas->xfer.tx_nbits=SPI_NBITS_SINGLE;
    smas->xfer.tx_buf = txb;
    smas->xfer.rx_nbits=SPI_NBITS_SINGLE;
    smas->xfer.rx_buf = rxb;
    smas->xfer.delay_usecs = 1;
    smas->xfer.len = len;
    smas->xfer.bits_per_word = 8;
    smas->xfer.speed_hz = smas->spi->max_speed_hz;
    spi_message_init(&smas->msg);
    spi_message_add_tail(&smas->xfer, &smas->msg);
    ret = spi_sync(smas->spi, &smas->msg);
    mutex_unlock(&dev_lock);
#endif
 	return ret;
}



/* 读数据
 * @return 成功:count, -1count太大，-2通讯失败, -3拷贝失败
 */
static ssize_t mas_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    int val, ret = 0;
    //MALOGD("start");       
    ret = mas_sync(stxb, srxb, count);
    if(ret) {
        MALOGW("mas_sync failed.");
        return -2;
    }
    ret = copy_to_user(buf, srxb, count);
    if(!ret) val = count;
    else {
        val = -3;
        MALOGW("copy_to_user failed.");
    }       
    //MALOGD("end.");
    return val;
}


static void mas_set_input(void) {
    struct input_dev *input = NULL;
    input = input_allocate_device();
    if (!input) {
        MALOGW("input_allocate_device failed.");
        return ;
    }
    set_bit(EV_KEY, input->evbit);
    set_bit(FINGERPRINT_SWIPE_UP, input->keybit); //单触
    set_bit(FINGERPRINT_SWIPE_DOWN, input->keybit);
    set_bit(FINGERPRINT_SWIPE_LEFT, input->keybit);
    set_bit(FINGERPRINT_SWIPE_RIGHT, input->keybit);
    set_bit(FINGERPRINT_TAP, input->keybit);
    set_bit(FINGERPRINT_DTAP, input->keybit);
    set_bit(FINGERPRINT_LONGPRESS, input->keybit);
    set_bit(KEY_POWER, input->keybit);
    set_bit(FINGERPRINT_POWER_ON, input->keybit); //zhangkaiyuan@wind-mobi.com 20180306
	
    input->name = MA_CHR_DEV_NAME;
    input->id.bustype = BUS_SPI;
    ret = input_register_device(input);
    if (ret) {
        input_free_device(input);
        MALOGW("failed to register input device.");
        return;
    }
    smas->input  = input;
}


static int init_interrupt(void);
static int deinit_interrupt(void);
static int mas_free_all(void);

//static int mas_ioctl (struct inode *node, struct file *filp, unsigned int cmd, uns igned long arg)           
//this function only supported while the linux kernel version under v2.6.36,while the kernel version under v2.6.36, use this line
static long mas_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    //MALOGF("start");
	int tmp;

    switch(cmd){
        case TIMEOUT_WAKELOCK:                                                       //延时锁    timeout lock
            __pm_wakeup_event(&gProcessWakeLock, 5*HZ);
            break;
        case SLEEP:                                                       //remove the process out of the runqueue
            smas->f_irq = 0;
			ret = wait_event_freezable(gWaitq, smas->f_irq != 0);
			break;
        case WAKEUP:                                                       //wake up, schedule the process into the runqueue
            smas->f_irq = 1;
            wake_up(&gWaitq);
            break;
        case ENABLE_CLK:
	    /*delete by guq@microarray.com.cn*/
            /*mas_enable_spi_clock(smas->spi);*/                                    //if the spi clock is not opening always, do this methods
            break;
        case DISABLE_CLK:
	    /*delete by guq@microarray.com.cn*/
            /*mas_disable_spi_clock(smas->spi);*/                                   //disable the spi clock
            break;
        case ENABLE_INTERRUPT:
            enable_irq(irq);                                                    //enable the irq,in fact, you can make irq enable always
            break;
        case DISABLE_INTERRUPT:
            disable_irq(irq);                                                    //disable the irq
            break;
        case TAP_DOWN:
            input_report_key(smas->input, FINGERPRINT_LONGPRESS, 1);
            input_sync(smas->input);                                                 //tap down
            break;
        case TAP_UP:
            input_report_key(smas->input, FINGERPRINT_LONGPRESS, 0);
            input_sync(smas->input);                                                     //tap up
            break;
        case SINGLE_TAP:
            input_report_key(smas->input, FINGERPRINT_TAP, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_TAP, 0);
            input_sync(smas->input);                                                       //single tap
            break;
        case DOUBLE_TAP:
            input_report_key(smas->input, FINGERPRINT_DTAP, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_DTAP, 0);
            input_sync(smas->input);                                              //double tap
            break;
        case LONG_TAP:
            input_report_key(smas->input, FINGERPRINT_LONGPRESS, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_LONGPRESS, 0);
            input_sync(smas->input);                                               //long tap
            break;
		case TAP_VIBRATE:
            input_report_key(smas->input, KEY_FP_VIBRATE, 1);
            input_sync(smas->input);
            input_report_key(smas->input, KEY_FP_VIBRATE, 0);
            input_sync(smas->input);                                               //long tap
            break;
	//zhangkaiyuan@wind-mobi.com 20180208 begin
	case MA_POWER_ON:
            input_report_key(smas->input, FINGERPRINT_POWER_ON, 1);
            input_sync(smas->input);

			//delay 10 ms to avoid screen on before key up,liuxiong@wind-mobi.com 20180919
			msleep(10);

            input_report_key(smas->input, FINGERPRINT_POWER_ON, 0);
            input_sync(smas->input);                                               //power on
            printk("wind-madev: input key KEY_FN_F8, early wakeup\n");
			break;	
	//zhangkaiyuan@wind-mobi.com 20180208 end	
        case MA_KEY_UP:
            input_report_key(smas->input, FINGERPRINT_SWIPE_UP, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_SWIPE_UP, 0);
            input_sync(smas->input);
            break;
        case MA_KEY_LEFT:
            input_report_key(smas->input, FINGERPRINT_SWIPE_LEFT, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_SWIPE_LEFT, 0);
            input_sync(smas->input);
           break;
        case MA_KEY_DOWN:
            input_report_key(smas->input, FINGERPRINT_SWIPE_DOWN, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_SWIPE_DOWN, 0);
            input_sync(smas->input);
            break;
        case MA_KEY_RIGHT:
            input_report_key(smas->input, FINGERPRINT_SWIPE_RIGHT, 1);
            input_sync(smas->input);
            input_report_key(smas->input, FINGERPRINT_SWIPE_RIGHT, 0);
            input_sync(smas->input);
            break;
        case SET_MODE:
            mutex_lock(&ioctl_lock);
            ret = copy_from_user(&ma_drv_reg, (unsigned int*)arg, sizeof(unsigned int));
            mutex_unlock(&ioctl_lock);
            break;
        case GET_MODE:
            mutex_lock(&ioctl_lock);
            ret = copy_to_user((unsigned int*)arg, &ma_drv_reg, sizeof(unsigned int));
            mutex_unlock(&ioctl_lock);
            break;
        case ENABLE_IRQ:
	    init_interrupt();
	    break;
	case DISABLE_IRQ:
	    deinit_interrupt();
	    break;
        case MA_IOC_GVER:
            mutex_lock(&ioctl_lock);
            //*((unsigned int*)arg) = MA_DRV_VERSION;
            mutex_unlock(&ioctl_lock);
            break;
		case SCREEN_ON:
			mas_switch_power(1);
			break;
		case SCREEN_OFF:
			mas_switch_power(0);
			break;
        case SET_SPI_SPEED:
            ret = copy_from_user(&ma_speed, (unsigned int*)arg, sizeof(unsigned int));
	    /*delete by guq@microarray.com.cn*/
	    /*ma_spi_change(smas->spi, ma_speed, 0);*/
            break;
        case WAIT_FACTORY_CMD:
            smas->u2_flag = 0;
           	ret = wait_event_freezable(U2_Waitq, smas->u2_flag != 0);
			break;
        case WAKEUP_FINGERPRINTD:
            smas->u2_flag = 1;
            wake_up(&U2_Waitq);
            break;
        case WAIT_FINGERPRINTD_RESPONSE:
            smas->u1_flag = 0;
            ret = wait_event_freezable(U1_Waitq,  smas->u1_flag != 0);
            mutex_lock(&ioctl_lock);
            tmp = copy_to_user((unsigned int*)arg, &ma_drv_reg, sizeof(unsigned int));
            mutex_unlock(&ioctl_lock);
			break;
        case WAKEUP_FACTORY_TEST_SEND_FINGERPRINTD_RESPONSE:
            mutex_lock(&ioctl_lock);
            ret = copy_from_user(&ma_drv_reg, (unsigned int*)arg, sizeof(unsigned int));
            mutex_unlock(&ioctl_lock);
            msleep(4);
            smas->u1_flag = 1;
            wake_up(&U1_Waitq);
            break;
		case WAIT_SCREEN_STATUS_CHANGE:
			screen_flag = 0;
			ret = wait_event_freezable(screenwaitq, screen_flag != 0);
			mutex_lock(&ioctl_lock);
			tmp = copy_to_user((unsigned int*)arg, &is_screen_on, sizeof(unsigned int));
			mutex_unlock(&ioctl_lock);
			break;
		case GET_INTERRUPT_STATUS:
			int_pin_state = mas_get_interrupt_gpio(0);
			mutex_lock(&ioctl_lock);
			tmp = copy_to_user((unsigned int*)arg, &int_pin_state, sizeof(unsigned int));
			mutex_unlock(&ioctl_lock);
			break;
		case GET_SCREEN_STATUS:
			mutex_lock(&ioctl_lock);
			ret = copy_to_user((unsigned int*)arg, &is_screen_on, sizeof(unsigned int));
			mutex_unlock(&ioctl_lock);
			break;
        case FREE_ALL:
            mutex_lock(&ioctl_lock);
            ret = copy_from_user(&free_check, (unsigned int*)arg, sizeof(unsigned int));
            mutex_unlock(&ioctl_lock);   
            mas_free_all();
            break;

        default:
            ret = -EINVAL;
            MALOGW("mas_ioctl no such cmd");
    }
    //MALOGF("end");
    return ret;
}

#ifdef CONFIG_COMPAT
static long mas_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int retval = 0;
    retval = filp->f_op->unlocked_ioctl(filp, cmd, arg);
    return retval;
}
#endif

#ifdef COMPATIBLE_VERSION3
int version3_ioctl(int cmd, int arg){
		int ret = 0;

		printd("%s: start cmd=0x%.3x arg=%d\n", __func__, cmd, arg);

		switch (cmd) {
				case IOCTL_DEBUG:
						sdeb = (u8) arg;
						break;
				case IOCTL_IRQ_ENABLE:
						break;
				case IOCTL_SPI_SPEED: 
						smas->spi->max_speed_hz = (u32) arg;
						spi_setup(smas->spi);
						break;
				case IOCTL_COVER_NUM:
						ret = COVER_NUM;
						break;
				case IOCTL_GET_VDATE:
						ret = 20160425;
						break;
				case IOCTL_CLR_INTF:
						smas->f_irq = FALSE;
						break;
				case IOCTL_GET_INTF:
						ret = smas->f_irq;
						break;
				case IOCTL_REPORT_FLAG:
						smas->f_repo = arg;
						break;
				case IOCTL_REPORT_KEY:
						input_report_key(smas->input, arg, 1);
						input_sync(smas->input);
						input_report_key(smas->input, arg, 0);
						input_sync(smas->input);
						break;
				case IOCTL_SET_WORK:
						smas->do_what = arg;
						break;
				case IOCTL_GET_WORK:
						ret = smas->do_what;
						break;
				case IOCTL_SET_VALUE:
						smas->value = arg;
						break;
				case IOCTL_GET_VALUE:
						ret = smas->value;
						break;
				case IOCTL_TRIGGER:
						smas->f_wake = TRUE;
						wake_up_interruptible(&drv_waitq);
						break;
				case IOCTL_WAKE_LOCK:
						if(!smas->wl.active)
								__pm_stay_awake(&smas->wl);
						break;
				case IOCTL_WAKE_UNLOCK:
						if(smas->wl.active)
								__pm_relax(&smas->wl);
						break;
				case IOCTL_KEY_DOWN:
						input_report_key(smas->input, KEY_F11 1);
						input_sync(smas->input);
						break;
				case IOCTL_KEY_UP:
						input_report_key(smas->input, KEY_F11, 0);
						input_sync(smas->input);
						break;
		}

		printd("%s: end. ret=%d f_irq=%d, f_repo=%d\n", __func__, ret, smas->f_irq, smas->f_repo);

		return ret;

}
#endif

/* 写数据
 * @return 成功:count, -1count太大，-2拷贝失败
 */
static ssize_t mas_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    int val = 0;
    //MALOGD("start");
    if(count==6) {                                                                              //cmd ioctl, old version used the write interface to do ioctl, this is only for the old version
        int cmd, arg;
        u8 tmp[6];
        ret = copy_from_user(tmp, buf, count);
        cmd = tmp[0];
        cmd <<= 8;
        cmd += tmp[1];
        arg = tmp[2];
        arg <<= 8;
        arg += tmp[3];
        arg <<= 8;
        arg += tmp[4];
        arg <<= 8;
        arg += tmp[5];
#ifdef COMPATIBLE_VERSION3
        val = (int)version3_ioctl(NULL, (unsigned int)cmd, (unsigned long)arg);
#endif
	} else {        
        memset(stxb, 0, FBUF);
        ret = copy_from_user(stxb, buf, count);     
        if(ret) {
            MALOGW("copy form user failed");
            val = -2;
        } else {
            val = count;
        }       
    }
    return val;
}
void * kernel_memaddr = NULL;
unsigned long kernel_memesize = 0;

int mas_mmap(struct file *filp, struct vm_area_struct *vma){
	unsigned long page;
	if ( !kernel_memaddr ) { 
			kernel_memaddr = kmalloc(128*1024, GFP_KERNEL);
			if( !kernel_memaddr ) { 
					return -1; 
			}   
	}  
    page = virt_to_phys((void *)kernel_memaddr) >> PAGE_SHIFT;
    vma->vm_page_prot=pgprot_noncached(vma->vm_page_prot);
    if( remap_pfn_range(vma, vma->vm_start, page, (vma->vm_end - vma->vm_start), 
                vma->vm_page_prot) )
        return -1;
    vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP);//VM_RESERVED;
    printk("remap_pfn_rang page:[%lu] ok.\n", page);
    return 0;
}

#ifdef COMPATIBLE_VERSION3
static unsigned int mas_poll(struct file *filp, struct poll_table_struct *wait) {
		unsigned int mask = 0;

		printd("%s: start. f_irq=%d f_repo=%d f_wake=%d\n",
						__func__, smas->f_irq, smas->f_repo, smas->f_wake);

		poll_wait(filp, &drv_waitq, wait);
		if(smas->f_irq && smas->f_repo) {
				smas->f_repo = FALSE;
				mask |= POLLIN | POLLRDNORM;
		} else if( smas->f_wake ) {
				smas->f_wake = FALSE;
				mask |= POLLPRI;
		}

		printd("%s: end. mask=%d\n", __func__, mask);

		return mask;
}
#endif

/*---------------------------------- fops ------------------------------------*/
static const struct file_operations sfops = {
    .owner = THIS_MODULE,
    .write = mas_write,
    .read = mas_read,
    .unlocked_ioctl = mas_ioctl,
    .mmap = mas_mmap,
    //.ioctl = mas_ioctl,       
    //using the previous line replacing the unlock_ioctl while the linux kernel under version2.6.36
#ifdef CONFIG_COMPAT
    .compat_ioctl = mas_compat_ioctl,
#endif
#ifdef COMPATIBLE_VERSION3
	.poll = mas_poll,
#endif
};
/*---------------------------------- fops end ---------------------------------*/

static int init_file_node(void)
{
    	int ret;
	    //MALOGF("start");
    	ret = alloc_chrdev_region(&sdev->idd, 0, 1, MA_CHR_DEV_NAME);
    	if(ret < 0)
    	{
        	MALOGW("alloc_chrdev_region error!");
        	return -1;
    	}
    	sdev->chd = cdev_alloc();
   	    if (!sdev->chd)
    	{
        	MALOGW("cdev_alloc error!");
        	return -1;
    	}
    	sdev->chd->owner = THIS_MODULE;
    	sdev->chd->ops = &sfops;
    	cdev_add(sdev->chd, sdev->idd, 1);
	    sdev->cls = class_create(THIS_MODULE, MA_CHR_DEV_NAME);
	    if (IS_ERR(sdev->cls)) {
		  MALOGE("class_create");
		  return -1;
	    }
	    sdev->dev = device_create(sdev->cls, NULL, sdev->idd, NULL, MA_CHR_FILE_NAME);
	    ret = IS_ERR(sdev->dev) ? PTR_ERR(sdev->dev) : 0;
	    if(ret){
	       	MALOGE("device_create");
	    }
	    //MALOGF("end");
        return 0;
}

static int deinit_file_node(void)
{
	cdev_del(sdev->chd);
	device_destroy(sdev->cls, sdev->idd);
	unregister_chrdev_region(sdev->idd, 1);
	return 0;
}

static int init_interrupt(void)
{
    const char*tname = MA_EINT_NAME;
    irq = mas_get_irq(NULL);
    if(irq<=0){
        ret = irq;
        MALOGE("mas_get_irq");
    }
#ifdef DOUBLE_EDGE_IRQ
	ret = request_irq(irq, mas_interrupt, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, tname, NULL);
#else
    ret = request_irq(irq, mas_interrupt, IRQF_TRIGGER_RISING, tname, NULL);
#endif
	if(ret<0){
        MALOGE("request_irq");
    }
    enable_irq_wake(irq);
    return ret;
}
static int deinit_interrupt(void)
{
    disable_irq(irq);
    free_irq(irq, NULL);
    return 0;
}


static int init_vars(void)
{
    sdev = kmalloc(sizeof(struct fprint_dev), GFP_KERNEL);
    smas = kmalloc(sizeof(struct fprint_spi), GFP_KERNEL);
    if (sdev==NULL || smas==NULL ) {
        MALOGW("smas kmalloc failed.");
        if(sdev!=NULL) kfree(sdev);
        if(smas!=NULL) kfree(smas);
        return -ENOMEM;
    }
    wakeup_source_init(&gProcessWakeLock, "microarray_process_wakelock");
    wakeup_source_init(&gIntWakeLock, "microarray_int_wakelock");
    INIT_WORK(&gWork, mas_work);
    gWorkq = create_singlethread_workqueue("mas_workqueue");
    if (!gWorkq) {
        MALOGW("create_single_workqueue error!");
        return -ENOMEM;
    }
    return 0;
}
static int deinit_vars(void)
{
    destroy_workqueue(gWorkq);
    wakeup_source_trash(&gProcessWakeLock);
    wakeup_source_trash(&gIntWakeLock);
    kfree(smas);
    kfree(sdev);
    return 0;
}

static int init_spi(struct spi_device *spi){
/*delete by the guq@microarray.com.cn*/
/*    smas->spi = spi;
    smas->spi->max_speed_hz = SPI_SPEED;
    spi_setup(spi);
    INIT_LIST_HEAD(&smas->dev_entry);
  */ 
    return 0;
}

static int deinit_spi(struct spi_device *spi){
/*delete by the guq@microarray.com.cn*/
/*    smas->spi = NULL;*/
    return 0;
}
/*
 * init_connect function to check whether the chip is microarray's 
 * @return 0 not 1 yes
 * param void
 */
int init_connect(void){
    int i;
    for(i=0; i<4; i++){
    	stxb[0] = 0x8c;
    	stxb[1] = 0xff;
    	stxb[2] = 0xff;
    	stxb[3] = 0xff;
    	mas_sync(stxb, srxb, 4);
    	msleep(8);
        stxb[0] = 0x00;
        stxb[1] = 0xff;
        stxb[2] = 0xff;
        stxb[3] = 0xff;
        ret = mas_sync(stxb, srxb, 4);
        if(ret!=0) MALOGW("do init_connect failed!");
		printk("guq srxb[3] = %d srxb[2] = %d\n", srxb[3], srxb[2]);
        if(srxb[3] == 0x41) return 1;
    }
    return 0;   
}


int deinit_connect(void){
    return 0;   
}

static int mas_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data){
	struct fb_event *evdata = data;
	unsigned int blank;
	if(event != FB_EVENT_BLANK) {
		return 0;
	}
	blank = *(int *)evdata->data;
	switch(blank){
		case FB_BLANK_UNBLANK:
			is_screen_on = 1;
			break;
		case FB_BLANK_POWERDOWN:
			is_screen_on = 0;
			break;
		default:
			break;
	}
	screen_flag = 1;
	wake_up(&screenwaitq);
	return 0;
}

static int init_notifier_call(void);
static int deinit_notifier_call(void);

static int init_notifier_call(){
	notifier.notifier_call = mas_fb_notifier_callback;
	fb_register_client(&notifier);
	is_screen_on = 1;
	return 0;
}

static int deinit_notifier_call(){
	fb_unregister_client(&notifier);
	return 0;
}

int mas_plat_probe(struct platform_device *pdev) {
    //MALOGD("start");
    ret = mas_finger_get_gpio_info(pdev);
    if(ret){
        MALOGE("mas_plat_probe do mas_finger_get_gpio_info");
    }
    ret = mas_finger_set_gpio_info(1);
    if(ret){
        MALOGE("mas_plat_probe do mas_finger_set_gpio_info");
    }
    //MALOGD("end");
    return ret;
}

int mas_plat_remove(struct platform_device *pdev) {
    mas_finger_set_gpio_info(0);
    return 0;
} 


int mas_probe(struct spi_device *spi) {

	int ret = 0;
	mas_do_some_for_probe(/*modify by guq@microarray.com.cn  original spi*/NULL);
	ret = init_vars(); 
	if(ret){
		goto err1;
	}
/* delete by guq@microarray.com.cn
    ret = init_interrupt();
    if(ret){
        goto err2;
    }
*/
	ret = init_file_node(); 
	if(ret){
	    goto err3;
	}
	ret = init_spi(/*modify by guq@microarray.com.cn original spi*/NULL);
	if(ret){
	    goto err4;
	}
#ifdef REE
	ret = init_connect();
#elif defined TEE
	ret = 1;
#endif
	if(ret == 0){//not chip 
	    compatible = 0;
	    goto err5;
	}
    mas_set_input();
    MALOGF("end");
   	ret = init_notifier_call();
	if(ret != 0){
		ret = -ENODEV;
		goto err6;
	}
    return ret;

err6:
	deinit_notifier_call();
err5:
    deinit_connect();
err4:
    deinit_spi(/*modify by guq@microarray.com.cn original spi*/NULL);
err3:
    deinit_file_node();
/*delete by guq@microarray.com.cn*/
/*err2:
    deinit_interrupt();*/
err1:
    deinit_vars();

    return ret;
}

int mas_remove(struct spi_device *spi) {
    deinit_file_node();
/*delete by guq@microarray.com.cn*/
    /*deinit_interrupt();*/
    deinit_vars();
    return 0;
}

static int mas_free_all(void)
{
    deinit_notifier_call();
    deinit_file_node();
    deinit_vars();
    if(kernel_memaddr) {
        kfree(kernel_memaddr);
    }
    //deinit_interrupt();
    mas_undo_some_for_probe();
    return 0;
}


static int __init mas_init(void)
{
    int ret = 0;
    MALOGF("start");
    printk("12345mafp\n");
    compatible = 1;
/*    ret = mas_get_platform();
    if(ret){
	   MALOGE("mas_get_platform");
    }
    if(compatible == 0){
        mas_remove_platform();
    }*/
/*modify by guq@microarray.com.cn for this qualcomm do not need any probe operation,do all probe operation in init duration*/
    ret = mas_probe(NULL);
    return ret;
}

static void __exit mas_exit(void)
{
    mas_remove(NULL);
}

module_init(mas_init);
module_exit(mas_exit);

MODULE_AUTHOR("Microarray");
MODULE_DESCRIPTION("Driver for microarray fingerprint sensor");
MODULE_LICENSE("GPL");
