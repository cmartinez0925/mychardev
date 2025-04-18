#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define CLASS_NAME          "myclass"
#define DEV_BUF_LEN         256
#define DEV_NAME            "mychardev"
#define IOCTL_MAGIC         'k'
#define SUCCESSFUL          0

#define IOCTL_RESET_BUF     _IO(IOCTL_MAGIC, 0)

static int                  data_available = 0;
static char                 dev_buf[DEV_BUF_LEN];
static size_t               dev_buf_size = 0; //The buffer will initially be empty
static dev_t                dev_num;
static struct cdev          mycdev;
static struct class*        myclass;
static struct device*       mydevice;

static DECLARE_WAIT_QUEUE_HEAD(wq);
static DEFINE_MUTEX(lock); //Defines and initalizes the mutex lock

//Author:      Chris Martinez
//Description: This is what will run when the device driver is open.
//             It will just print a message to notify that it's open.
//Date:        14 April 2025
//Version:     1.0
static int dev_open(struct inode* inode, struct file* file) {
    pr_info("mychardev: The device is now opening...\n");
    return 0;
}

//Author:      Chris Martinez
//Description: This is what will run when the device driver is release.
//             It will just print a message to notify that it's release.
//Date:        14 April 2025
//Version:     1.0
static int dev_release(struct inode* inode, struct file* file) {
    pr_info("mychardev: The device is now being release...\n");
    return 0;
}

//Author:      Chris Martinez
//Description: This will copy data from the device's buffer to the user's buffer.
//             Returns the amount of data that has been copied to the user.
//Date:        15 April 2025
//Version:     1.0
static ssize_t dev_read(struct file* file, char __user* user_buf, size_t amt_to_copy, loff_t* dev_offset) {
    size_t amt_copied = 0;

    //If the offset exceeds the size of the dev's buffer, then there is no more data to copy 
    if (*dev_offset >= dev_buf_size) {
        return amt_copied;
    }

    //Set up a mutex, so that there is no race condition while data is being read
    if (mutex_lock_interruptible(&lock) != SUCCESSFUL) {
        return -ERESTARTSYS;
    }

    //We need to determine what data is remaining in the dev's buffer that needs to be copied to the user
    //Then if amt_to_copy exceeds the remaining amt, we must adjust
    size_t amt_data_remaining = dev_buf_size - *dev_offset;
    if (amt_to_copy > amt_data_remaining) {
        amt_to_copy = amt_data_remaining;
    }
    
    //If all the data not copied to user, then there is an issue
    //We need to unlock the mutex, and exit the function with an error
    if (copy_to_user(user_buf, dev_buf, amt_to_copy) != SUCCESSFUL) {
        mutex_unlock(&lock);
        return -EFAULT;
    }

    //Update the buffer's offset, amt_copied and set the data_available to 0
    *dev_offset += amt_to_copy;
    amt_copied = amt_to_copy;
    data_available = 0;

    //Unlock the mutex, and return the amount copied to user as we are done copying to the user's buffer
    mutex_unlock(&lock);
    return amt_copied;    
}

//Author:      Chris Martinez
//Description: This will write data to the device's buffer from the user's buffer.
//             Returns the amount of data that has been written to the user.
//Date:        15 April 2025
//Version:     1.0
static ssize_t dev_write(struct file* file, const char __user* user_buf, size_t amt_to_write, loff_t* dev_offset) {
    //The amt_to_write should not exceed the total dev_buffer_len, if so exit with error
    if (amt_to_write > dev_buf_size) {
        return -EINVAL;
    }

    //Set the mutex, so no race condition happens while a write operation is happening
    if (mutex_lock_interruptible(&lock) != SUCCESSFUL) {
        return -ERESTARTSYS;
    }

    //Write the data from the user to dev, if not successful unlock mutex and return error code
    if (copy_from_user(dev_buf, user_buf, amt_to_write) != SUCCESSFUL) {
        mutex_unlock(&lock);
        return -EFAULT;
    }

    //Update the dev_buf_size, since there is new data. Must also set data_available to 1 for the poll function
    dev_buf_size = amt_to_write;
    data_available = 1;

    mutex_unlock(&lock); //unlock the mutex
    wake_up_interruptible(&wq); //wake up the wait queue now that there is data available
    return amt_to_write;
}

//Author:      Chris Martinez
//Description: This will handle any poll event for the device driver
//Date:        16 April 2025
//Version:     1.0
static unsigned int dev_poll(struct file* file, struct poll_table_struct* wait) {
    unsigned int mask = 0; //this will keep track of the boolean poll values
    poll_wait(file, &wq, wait); //adds the wq to the queue
    if (data_available) {
        mask = mask | POLLIN | POLLRDNORM;
    }

    return mask;
}

//Author:      Chris Martinez
//Description: This will reset the buffers
//Date:        16 April 2025
//Version:     1.0
static long dev_ioctl(struct file* file, unsigned int cmd, unsigned long args) {
    switch (cmd) {
        case IOCTL_RESET_BUF:
            mutex_lock(&lock);
            memset(dev_buf, 0, DEV_BUF_LEN); //reset the dev_buf to zero
            dev_buf_size = 0; //if dev_buf is zeroize then reset the buf_size to zero
            data_available = 0; //No data in buf, so no data_available
            mutex_unlock(&lock);
            pr_info("mychardev: The device's buffer has been resetted to zero via ioctl.\n");
            break;
        default:
            return -EINVAL;
    }
    return SUCCESSFUL;
}

//Author:      Chris Martinez
//Description: This creates the file operations structure to pass the
//             functions needs to operate the device driver
//Date:        13 April 2025
//Version:     1.0
static struct file_operations file_ops = {
    .owner           = THIS_MODULE,
    .open            = dev_open,
    .release         = dev_release,
    .read            = dev_read,
    .write           = dev_write,
    .poll            = dev_poll,
    .unlocked_ioctl  = dev_ioctl
};

//Author:      Chris Martinez
//Description: Will create the char driver within the init
//Date:        13 April 2025
//Version:     1.0
static int __init mychardev_init(void) {
    int ret;

    //First we must allocate a device number for the device driver
    //If it fails (< 0), then we must notified the user
    //If it succeeds, dev_num is loaded with the device number that is allocated
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEV_NAME);
    if (ret < SUCCESSFUL) {
        pr_alert("mychardev: Unable to allocate a major number for device.\n");
        return ret;
    }

    //Initialize the cdev structure
    cdev_init(&mycdev, &file_ops);

    //Add the char device to the system
    //If it fails (< 0), then we must notified the user
    //If succeeds, device added to the system
    ret = cdev_add(&mycdev, dev_num, 1);
    if (ret < SUCCESSFUL) {
        pr_alert("mychardev: Unable to add device to the system.\n");
        pr_alert("mychardev: Unregistering the device number....\n");
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    //Will need to create a class
    //Will need to check if it fails via IS_ERR
    // myclass = class_create(THIS_MODULE, CLASS_NAME);
    myclass = class_create(CLASS_NAME);
    if (IS_ERR(myclass)) {
        pr_alert("mychardev: Unable to create a class.\n");
        pr_alert("mychardev: Deleting cdev...\n");
        cdev_del(&mycdev);
        pr_alert("mychardev: Unregistering the device number...\n");
        unregister_chrdev_region(dev_num, 1);
    }

    //Will need to create the device to /dev
    //Will need to check if if fails via IS_ERR
    mydevice = device_create(myclass, NULL, dev_num, NULL, DEV_NAME);
    if (IS_ERR(mydevice)) {
        pr_alert("mychardev: Unable to create a device to /dev.\n");
        pr_alert("mychardev: Deleting the class...\n");
        class_destroy(myclass);
        pr_alert("mychardev: Deleting cdev...\n");
        cdev_del(&mycdev);
        pr_alert("mychardev: Unregistering the device number...\n");
        unregister_chrdev_region(dev_num, 1);
    }

    pr_info("mychardev: The Device Driver Module has been loaded to -> /dev/%s\n", DEV_NAME);
    return 0;
}

//Author:      Chris Martinez
//Description: Will unload the device driver module
//Date:        14 April 2025
//Version:     1.0
static void __exit mychardev_exit(void) {
    device_destroy(myclass, dev_num);
    pr_alert("mychardev: Removing device from /dev...\n");
    class_destroy(myclass);
    pr_alert("mychardev: Deleting the class...\n");
    cdev_del(&mycdev);
    pr_alert("mychardev: Deleting cdev...\n");
    unregister_chrdev_region(dev_num, 1);
    pr_alert("mychardev: Unregistering the device number...\n");
    pr_info("mychardev: The Device Driver Module has been unloaded.\n");
}

module_init(mychardev_init);
module_exit(mychardev_exit);

MODULE_AUTHOR("Chris Martinez");
MODULE_DESCRIPTION("My First Character Driver Device");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");