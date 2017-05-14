#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>


#define UNIQUEFS_DEFAULT_MODE	0755
#define UNIQUEFS_NAME_MAX		32
#define MAX_NB_FILES 			1

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("unique file system");
MODULE_AUTHOR("Group 1");

struct file_data{
	loff_t size;
	char *data;
};

// resize to data field of file_data to something bigger or equal than newsize
// only copying the data up to the offset to_copy
// return false if the reallocation was impossible and true otherwise
static bool grow(struct file_data *file_data, loff_t newsize, loff_t to_copy){
	size_t i = 0, k = 2;
	char *tmp;

	for(; k * (file_data->size) < newsize; ++k){}
	tmp = vmalloc(k * (file_data->size));
	if (tmp == NULL){
		return false;
	}
	file_data->size = k * (file_data->size);
	for(; i < to_copy; ++i){
		tmp[i] = file_data->data[i];
	}
	vfree(file_data->data);
	file_data->data = tmp;
	return true;
}

static const struct inode_operations uniquefs_dir_inode_operations;

static struct super_operations uniquefs_ops = {
	.drop_inode	= generic_delete_inode,
};


static const struct inode_operations uniquefs_file_inode_operations = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
}; //For a virtual FS, this is sufficient

static ssize_t uniquefs_read(struct file *file, char __user *to, size_t size, loff_t *offset){
	struct file_data* file_data = file->f_inode->i_private;
	ssize_t copied, buf_size;

	inode_lock(file->f_inode);
	buf_size  = file->f_inode->i_size - *offset;
	if (size > buf_size){
		size = buf_size;
	}
	copied = size - copy_to_user(to,file_data->data + *offset,size);
	*offset += copied;
	inode_unlock(file->f_inode);
	return copied;
}

static ssize_t uniquefs_write(struct file *file, const char __user *from, size_t  size, loff_t *offset){
	struct file_data* file_data = file->f_inode->i_private;
	ssize_t copied, buf_max_size;

	inode_lock(file->f_inode);
	buf_max_size  = file_data->size - *offset;
	if (size > buf_max_size){
		if (!grow(file_data, size + *offset, file->f_inode->i_size)){
			inode_unlock(file->f_inode);
			return -ENOMEM;
		}
	}
	copied = size - copy_from_user(file_data->data + *offset, from, size);
	*offset += copied;
	if(file->f_inode->i_size < *offset){
		file->f_inode->i_size = *offset;
	}
	inode_unlock(file->f_inode);
	return copied;
}

static const struct file_operations uniquefs_file_operations = {
	.read 		= uniquefs_read,
	.write 		= uniquefs_write,
	.mmap		= generic_file_mmap,
	.fsync		= noop_fsync,
	.llseek		= generic_file_llseek,
};

struct inode *uniquefs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);
	struct file_data *tmp;

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &uniquefs_file_inode_operations;
			inode->i_fop = &uniquefs_file_operations;
			inode->i_private = vmalloc(sizeof(struct file_data));
			if (inode->i_private == NULL){
				drop_nlink(inode);
				return NULL;
			}
			tmp = inode->i_private;
			inode->i_size = 0; // probably already done somewhere else
			tmp->size = PAGE_CACHE_SIZE;
			tmp->data = vmalloc(tmp->size);
			if (inode->i_private == NULL){
				vfree(inode->i_private);
				drop_nlink(inode);
				return NULL;
			}
			break;
		case S_IFDIR:
			inode->i_op = &uniquefs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations; //Generic dir
			inode->i_private = (void*) 0;
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
	int error;
	if (((int) (dir->i_private)) >= MAX_NB_FILES){
		return -EPERM;
	}
	if (dentry->d_name.len > UNIQUEFS_NAME_MAX){
		return -ENAMETOOLONG;
	}
	error = uniquefs_mknod(dir, dentry, mode | S_IFREG, 0);
	if (error == 0){
		++(dir->i_private);
	}
	return error;
}

static int uniquefs_unlink(struct inode *dir,struct dentry *dentry)
{
	struct file_data* tmp = dentry->d_inode->i_private;
	vfree(tmp->data);
	vfree(tmp);
	--(dir->i_private);
	return simple_unlink(dir, dentry);
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
	.unlink		= uniquefs_unlink,
	.mknod		= uniquefs_mknod,
	.rename		= uniquefs_rename,
};

int uniquefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = UNIQUEFS_MAGIC;
	sb->s_op = &uniquefs_ops;
	inode = uniquefs_get_inode(sb, NULL, S_IFDIR | UNIQUEFS_DEFAULT_MODE, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root){
		return -ENOMEM;
	}
	return 0;
}

struct dentry *uniquefs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, uniquefs_fill_super);
};

static struct file_system_type uniquefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "uniquefs",
	.mount = uniquefs_mount, //To implement, mount a device to a location. This function return a "dentry", a directory. The root directory.
	.kill_sb = kill_litter_super,
	.fs_flags = FS_USERNS_MOUNT, //From fs.h:1861 Allow user. Real FS backed by a device use FS_REQUIRES_DEV
};

int __init uniquefs_init(void)
{
	return register_filesystem(&uniquefs_fs_type);
}

void __exit uniquefs_exit(void)
{
    unregister_filesystem(&uniquefs_fs_type);
}

module_init(uniquefs_init);
module_exit(uniquefs_exit);
