#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/serio.h>

#define NUNCHUK_I2C_REPORT_SIZE 6

struct nunchuk_report {
	u8	joy_x;
	u8	joy_y;
	u16	acc_x;
	u16	acc_y;
	u16	acc_z;
	u8	C;
	u8	Z;
};

struct key_report {
	bool up_pressed;
	bool down_pressed;
	bool left_pressed;
	bool right_pressed;
	bool ctrl_pressed;
	bool space_pressed;
};

struct nunchuk_device {
	struct i2c_client 	*client;
	struct serio		*serio;
	struct workqueue_struct	*wq;
	struct delayed_work	dwork;
	int			refn;
	u8			report_buf[NUNCHUK_I2C_REPORT_SIZE];
	struct nunchuk_report	report;
	struct key_report	key_report;
};

static bool inhibit = true;

#define DEADZONE 50

static atomic_t refcount = ATOMIC_INIT(0);

static inline struct nunchuk_device *to_nunchuk_device(struct work_struct *work)
{
	return container_of(to_delayed_work(work), struct nunchuk_device, dwork);
}

static void i2c_smbus_read_byte_array(struct i2c_client *client, u8 length, u8 *values)
{
	u8 i;
	for (i = 0; i < length; i++) {
		values[i] = i2c_smbus_read_byte(client);
	}
}

static void nunchuk_init_reporting(struct nunchuk_device *nunchuk)
{
	i2c_smbus_write_byte_data(nunchuk->client, 0xF0, 0x55);
	i2c_smbus_write_byte_data(nunchuk->client, 0xFB, 0x00);
	/* Prepare the next reading */
	i2c_smbus_write_byte(nunchuk->client, 0x00);
}

static void nunchuk_correct_report(struct nunchuk_device *nunchuk)
{
	struct nunchuk_report *report = &nunchuk->report;
	u8 *report_buf = nunchuk->report_buf;

	report->joy_x = report_buf[0];
	report->joy_y = report_buf[1];
	report->acc_x = (((report_buf[5]>>2) & 0b11) | ((u16)report_buf[2])<<2);
	report->acc_y = (((report_buf[5]>>4) & 0b11) | ((u16)report_buf[3])<<2);
	report->acc_z = (((report_buf[5]>>6) & 0b11) | ((u16)report_buf[4])<<2);
	report->Z = (~report_buf[5]) & 0b1;
	report->C = ((~report_buf[5])>>1) & 0b1;
}

static void serio_update(struct serio *serio, bool old, bool new, bool escaped, u8 code) {
	if (old == new)
		return;

	if (escaped)
		serio_interrupt(serio, 0xe0, 0);

	if (!new)
		serio_interrupt(serio, 0xf0, 0);

	serio_interrupt(serio, code, 0);
}

// Dirty, no hysteresis
static void nunchuk_do_keyboard(struct nunchuk_device *nunchuk) {
	bool up_pressed = false;
	bool down_pressed = false;
	bool left_pressed = false;
	bool right_pressed = false;
	bool ctrl_pressed = false;
	bool space_pressed = false;

	struct nunchuk_report *report = &nunchuk->report;
	up_pressed = report->joy_y > 127 + DEADZONE;
	down_pressed = report->joy_y < 127 - DEADZONE;
	right_pressed = report->joy_x > 127 + DEADZONE;
	left_pressed = report->joy_x < 127 - DEADZONE;
	ctrl_pressed = report->C;
	space_pressed = report->Z;

	if (!inhibit) {
		serio_update(nunchuk->serio, nunchuk->key_report.up_pressed, up_pressed, true, 0x75);
		serio_update(nunchuk->serio, nunchuk->key_report.down_pressed, down_pressed, true, 0x72);
		serio_update(nunchuk->serio, nunchuk->key_report.left_pressed, left_pressed, true, 0x6b);
		serio_update(nunchuk->serio, nunchuk->key_report.right_pressed, right_pressed, true, 0x74);
		serio_update(nunchuk->serio, nunchuk->key_report.ctrl_pressed, ctrl_pressed, false, 0x14);
		serio_update(nunchuk->serio, nunchuk->key_report.space_pressed, space_pressed, false, 0x29);
	}

	nunchuk->key_report.up_pressed = up_pressed;
	nunchuk->key_report.down_pressed = down_pressed;
	nunchuk->key_report.left_pressed = left_pressed;
	nunchuk->key_report.right_pressed = right_pressed;
	nunchuk->key_report.ctrl_pressed = ctrl_pressed;
	nunchuk->key_report.space_pressed = space_pressed;
}

static void workqueue_function(struct work_struct *work)
{
	struct nunchuk_device *nunchuk;
	nunchuk = to_nunchuk_device(work);

	i2c_smbus_read_byte_array(nunchuk->client,
		NUNCHUK_I2C_REPORT_SIZE, nunchuk->report_buf);

	nunchuk_correct_report(nunchuk);

	nunchuk_do_keyboard(nunchuk);

	/* Prepare the next reading */
	i2c_smbus_write_byte(nunchuk->client, 0x00);

	queue_delayed_work(nunchuk->wq, &nunchuk->dwork, msecs_to_jiffies(10));
}

static int nunchuk_open(struct serio *serio)
{
	inhibit = false;
	return 0;
}

static void nunchuk_close(struct serio *serio)
{
	
}

static int nunchuk_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct nunchuk_device *nunchuk;
	int err;
	struct serio *serio = NULL;

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE |
			I2C_FUNC_SMBUS_WRITE_BYTE_DATA)) {
		printk(KERN_ERR "nunchuk_i2c_probe: "
			"unsupported adapter\n");
		return -EIO;
	} else {
		printk(KERN_DEBUG "nunchuk_i2c : nunchuk found.\n");
	}

	nunchuk = kzalloc(sizeof(*nunchuk), GFP_KERNEL);
	if (nunchuk == NULL) {
		return -ENOMEM;
	}
	serio = kzalloc(sizeof(struct serio), GFP_KERNEL);
	if (!serio) {		
		err = -ENOMEM;
		goto err_freenun;
	}

	nunchuk->client = client;

	nunchuk->refn = atomic_inc_return(&refcount) - 1;

	serio->id.type = SERIO_8042;
	serio->open = nunchuk_open;
	serio->close = nunchuk_close;
	serio->write = NULL;
	serio->port_data = nunchuk;
	serio->dev.parent = &client->dev;
	strlcpy(serio->name, dev_name(&client->dev), sizeof(serio->name));
	strlcpy(serio->phys, dev_name(&client->dev), sizeof(serio->phys));
	nunchuk->serio = serio;

	nunchuk_init_reporting(nunchuk);

	nunchuk->wq = create_singlethread_workqueue("nunchuk");
	INIT_DELAYED_WORK(&nunchuk->dwork, workqueue_function);

	i2c_set_clientdata(client, nunchuk);

	queue_delayed_work(nunchuk->wq, &nunchuk->dwork, msecs_to_jiffies(50));

	serio_register_port(serio);

	return 0;

err_freedev:
	kfree(serio);
err_freenun:
	kfree(nunchuk);
	return err;
}

static int nunchuk_i2c_remove(struct i2c_client *client)
{
	struct nunchuk_device *nunchuk;

	nunchuk = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&nunchuk->dwork);
	flush_workqueue(nunchuk->wq);
	destroy_workqueue(nunchuk->wq);

	serio_unregister_port(nunchuk->serio);

	kfree(nunchuk->serio);
	kfree(nunchuk);

	return 0;
}


static const struct i2c_device_id nunchuk_i2c_idtable[] = {
	{"nunchuk-i2c", 0},
	{ }
};

MODULE_DEVICE_TABLE(i2c, nunchuk_i2c_idtable);

static const struct of_device_id nunchuk_of_match[] = {
	{ .compatible = "nintendo,nunchuk" },
	{ }
};
MODULE_DEVICE_TABLE(of, nunchuk_of_match);

static struct i2c_driver nunchuk_i2c_driver = {
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "nunchuk-i2c",
		.of_match_table = nunchuk_of_match,
	},
	.id_table	= nunchuk_i2c_idtable,
	.probe		= nunchuk_i2c_probe,
	.remove		= nunchuk_i2c_remove,
};

static int __init nunchuk_i2c_init(void)
{
	int err;

	err = i2c_add_driver(&nunchuk_i2c_driver);
	if (err) {
		printk(KERN_ERR "nunchuk_i2c_init: "
			"failed to add I2C driver (%d)\n", err);
		return err;
	}
	printk(KERN_ALERT "nunchuk_i2c : Init ok.\n");	

	return 0;
}

static void __exit nunchuk_i2c_exit(void)
{
	i2c_del_driver(&nunchuk_i2c_driver);
}

module_init(nunchuk_i2c_init);
module_exit(nunchuk_i2c_exit);

MODULE_AUTHOR("Sergi Granell");
MODULE_AUTHOR("Andrea Campanella");
MODULE_AUTHOR("Tobias Schramm");
MODULE_DESCRIPTION("Nunchuk I2C input driver");
MODULE_LICENSE("GPL");
