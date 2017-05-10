#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/pagemap.h>

#define UNIQUEFS_DEFAULT_MODE	0755
#define UNIQUEFS_NAME_MAX		32

static const struct inode_operations uniquefs_dir_inode_operations;

static struct super_operations uniquefs_ops = {
	.drop_inode	= generic_delete_inode,
};

static const struct inode_operations uniquefs_file_inode_operations = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
}; //For a virtual FS, this is sufficient


static ssize_t uniquefs_read(struct file *file, char __user *buffer, size_t size, loff_t *offset)
{
	char* buf = "Hello\n";
	size_t copied;
	size_t buf_size = 7*sizeof(char) - *offset;

	if (buf_size > size){
		buf_size = size;
	}
	copied = buf_size - copy_to_user(buffer,buf + *offset,buf_size);
	*offset += copied;
	return copied;
}
	

//ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);


static const struct file_operations uniquefs_file_operations = {
	.read = uniquefs_read
	//.write = ...
	//.mmap = ...
};

struct inode *uniquefs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &uniquefs_file_inode_operations;
			inode->i_fop = &uniquefs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &uniquefs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations; //Generic dir
			break;
		}
	}
	return inode;
}

static int uniquefs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = uniquefs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	}
	return error;
}


static int uniquefs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	if (dentry->d_name.len > UNIQUEFS_NAME_MAX){
		return -ENOSPC;
	}
	return uniquefs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int uniquefs_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry)
{
	if (new_dentry->d_name.len > UNIQUEFS_NAME_MAX){
		return -ENAMETOOLONG;
	}
	return simple_rename(old_dir, old_dentry, new_dir, new_dentry);
}

static const struct inode_operations uniquefs_dir_inode_operations = {
	.create		= uniquefs_create,
	.lookup		= simple_lookup,
	.unlink		= simple_unlink,
	.mknod		= uniquefs_mknod,
	.rename		= uniquefs_rename,
};

int uniquefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;

	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = UNIQUEFS_MAGIC;
	sb->s_op = &uniquefs_ops;
	inode = uniquefs_get_inode(sb, NULL, S_IFDIR | UNIQUEFS_DEFAULT_MODE, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

struct dentry *uniquefs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, uniquefs_fill_super);
};

static void uniquefs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
}

static struct file_system_type uniquefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "uniquefs",
	.mount = uniquefs_mount, //To implement, mount a device to a location. This function return a "dentry", a directory. The root directory.
	.kill_sb = uniquefs_kill_sb,
	.fs_flags = FS_USERNS_MOUNT, //From fs.h:1861 Allow user. Real FS backed by a device use FS_REQUIRES_DEV
};

int __init init_uniquefs_fs(void)
{
	return register_filesystem(&uniquefs_fs_type);
}

fs_initcall(init_uniquefs_fs);