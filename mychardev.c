#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>

#define DEVICE_NAME "mychardev"
#define CLASS_NAME  "myclass"
#define IOCTL_MAGIC 'k'
#define IOCTL_RESET_BUFFER _IO(IOCTL_MAGIC, 0)

#define BUF_LEN         256
#define MUTEX_SUCCESS   0

static dev_t device_num;
static struct cdev my_cdev;
static struct class* my_class;
static struct device* my_device;

static char device_buffer[BUF_LEN];
static size_t device_buffer_size = 0;
static DECLARE_WAIT_QUEUE_HEAD(wq);
static int data_available = 0;
static DEFINE_MUTEX(device_mutex); //define and initialize the mutex
 
//Open
static int device_open(struct inode* inode, struct file* file) {
    pr_info("mychardev: The device is now opened.\n");
    return 0;
}

//Release
static int device_release(struct inode* inode, struct file* file) {
    pr_info("mychardev: The device is now released.\n");
    return 0;
}

//Read
static ssize_t device_read(struct file* file, char __user* user_buffer, size_t amount_to_read, loff_t* device_buffer_offset) {
    ssize_t ret;

    //the offset within device buffer is greater than the size of device buffer
    //return 0, as the device buffer has been completely read
    if (*device_buffer_offset >= device_buffer_size) {
        return 0;
    }

    //on success returns 0
    if (mutex_lock_interruptible(&device_mutex) != MUTEX_SUCCESS) {
        return -ERESTARTSYS;
    }

    //if length exceeds what's left of the device buffer, 
    // then resize the length to what's left of the device buffer
    size_t remaining_dev_buffer_space = device_buffer_size - *device_buffer_offset;
    if (amount_to_read > remaining_dev_buffer_space) {
        amount_to_read = remaining_dev_buffer_space;
    }
    
    //copy_to_user returns amount not copied, on success should be 0
    if (copy_to_user(user_buffer, device_buffer + *device_buffer_offset, amount_to_read) != 0) {
        mutex_unlock(&device_mutex);
        return -EFAULT;
    }

    //increase the device buffer offset by the amount of data you just read
    *device_buffer_offset += amount_to_read;
    ret = amount_to_read;

    data_available = 0;
    mutex_unlock(&device_mutex); //unlock the current mutex
    return ret; //return the amount of data user just read from the device
}

static ssize_t device_write(struct file* file, const char __user* user_buffer, size_t amount_to_write, loff_t* device_buffer_offset) {
    
    //Return error if the amount user is writing to kernel buffer exceeds the kernel buffer length
    if (amount_to_write > BUF_LEN) {
        return -EINVAL;
    }

    //set the mutex, returns 0 if successful
    if (mutex_lock_interruptible(&device_mutex) != 0) {
        return -ERESTARTSYS;
    }

    //write to device buffer from the user'buffer
    if (copy_from_user(device_buffer, user_buffer, amount_to_write)) {
        mutex_unlock(&device_mutex);
        return -EFAULT;
    }
    
    //the size of the device's buffer needs to be updated based on the amount just written
    device_buffer_size = amount_to_write;
    data_available = 1; //data is now avilable since it lays in the device's buffer

    mutex_unlock(&device_mutex); //unlock the mutex
    wake_up_interruptible(&wq); //wake up the write queue based on this action
    
    return amount_to_write; //return the amount of data that was written to the device

}

static long device_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    //handle the different types of commands using switch statments
    switch (cmd) {
        case IOCTL_RESET_BUFFER:
            mutex_lock(&device_mutex);
            memset(device_buffer, 0, BUF_LEN); //zero-out the device's buffer
            device_buffer_size = 0; //the size of the device's buffer is 0, since no data is in the buffer anymore
            data_available = 0; //device buffer being zeroed means no data is available
            mutex_unlock(&device_mutex);
            pr_info("mychardev: The device's buffer has been resetted to zero via ioctl.\n");
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static __poll_t device_poll(struct file* file, struct poll_table_struct* wait) {
    __poll_t mask = 0; //this will keep track of the boolean poll values

    poll_wait(file, &wq, wait); //add the wq to the queue

    if (data_available) {
        mask = mask | POLLIN | POLLRDNORM;
    }

    return mask;
}

//create a file_operations struct and link the functions to it
static struct file_operations file_ops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .write = device_write,
    .unlocked_ioctl = device_ioctl,
    .poll = device_poll,
};

static int __init myfirstchar_init(void) {
    int ret;

    ret = alloc_chrdev_region(&device_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_alert("mychardev: Failed to allocate a major number\n");
        return ret;
    }

    cdev_init(&my_cdev, &file_ops);
    ret = cdev_add(&my_cdev, device_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(device_num, 1);
        return ret;
    }

    // my_class = class_create(THIS_MODULE, CLASS_NAME);
    my_class = class_create(CLASS_NAME);
    if (IS_ERR(my_class)) {
        cdev_del(&my_cdev);
        unregister_chrdev_region(device_num, 1);
        return PTR_ERR(my_class);
    }

    my_device = device_create(my_class, NULL, device_num, NULL, DEVICE_NAME);
    if (IS_ERR(my_device)) {
        class_destroy(my_class);
        cdev_del(&my_cdev);
        unregister_chrdev_region(device_num, 1);
        return PTR_ERR(my_device);
    }


    pr_info("mychardev: Module loaded with device /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit myfirstchar_exit(void) {
    device_destroy(my_class, device_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(device_num, 1);
    pr_info("mychardev: Module unloaded\n");
}

module_init(myfirstchar_init);
module_exit(myfirstchar_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Martinez");
MODULE_DESCRIPTION("A simple linux kernel char driver with mulitple file operations");
MODULE_VERSION("1.0");