/*
 * fs/hmfs/acl.c
 *
 * Copyright (c) 2015 RadLab SJTU
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Portions of this code from linux/fs/ext2/acl.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "hmfs_fs.h"
#include "hmfs.h"
#include "xattr.h"
#include "acl.h"
/**
 * 返回count个acl_entry对应的字节数
 */
static inline size_t hmfs_acl_size(int count)
{
	if (count <= 4) {
		return ACL_HEADER_SIZE + count * ACL_SHORT_ENTRY_SIZE;
	} else {
		return ACL_HEADER_SIZE + 4 * ACL_SHORT_ENTRY_SIZE + 
					(count - 4) * ACL_ENTRY_SIZE;
	}
}
/**
 * 返回size个字节对应的访问控制表entry数
 */
static inline int hmfs_acl_count(size_t size)
{
	ssize_t s;
	size -= ACL_HEADER_SIZE;
	s = size - 4 * ACL_SHORT_ENTRY_SIZE;

	if (s < 0) {
		if (size % ACL_SHORT_ENTRY_SIZE)
			return -1;
		return size / ACL_SHORT_ENTRY_SIZE;
	} else {
		if (s % ACL_ENTRY_SIZE)
			return -1;
		return s / ACL_ENTRY_SIZE + 4;
	}
}
/**
 * 获取文件访问控制表的地址
 * @inode为该文件inode
 * 成功则返回访问控制表地址
 * 否则返回NULL
 */
static void *get_acl_block(struct inode *inode)
{
	struct hmfs_sb_info *sbi = HMFS_I_SB(inode);
	block_t acl_addr;
	struct hmfs_inode *inode_block;

	inode_block = get_node(sbi, inode->i_ino);
	if (IS_ERR(inode_block))
		return NULL;

	acl_addr = le64_to_cpu(inode_block->i_acl_addr);
	if (acl_addr)
		return ADDR(sbi, acl_addr);
	return NULL;
}
/**
 * 读取访问控制表内容
 * @base_addr为要读取的ACL的起始地址
 * @size为要读取的字节数
 * 返回值为指向存储了访问控制表内容的新建ACL表的指针
 */
static struct posix_acl *hmfs_read_acl(const char *base_addr, size_t size)
{
	int i, count;
	struct posix_acl *acl;
	struct hmfs_acl_entry *acl_entry = ACL_ENTRY(base_addr);
	const char *end = base_addr + size;

	count = hmfs_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (count == 0)
		return NULL;

	acl = posix_acl_alloc(count, GFP_NOFS);
	if (!acl)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++) {
		if ((char *)acl_entry > end)
			goto fail;

		acl->a_entries[i].e_tag = le16_to_cpu(acl_entry->e_tag);
		acl->a_entries[i].e_perm = le16_to_cpu(acl_entry->e_perm);
		
		switch (acl->a_entries[i].e_tag) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			acl_entry = ACL_ENTRY(JUMP(acl_entry, ACL_SHORT_ENTRY_SIZE));
			break;

		case ACL_USER:
			acl->a_entries[i].e_uid = make_kuid(&init_user_ns, 
											le32_to_cpu(acl_entry->e_id));
			acl_entry = ACL_ENTRY(JUMP(acl_entry, ACL_ENTRY_SIZE));
			break;

		case ACL_GROUP:
			acl->a_entries[i].e_gid = make_kgid(&init_user_ns,
											le32_to_cpu(acl_entry->e_id));
			acl_entry = ACL_ENTRY(JUMP(acl_entry, ACL_ENTRY_SIZE));
			break;
		default:
			goto fail;
		}
	}
	if ((char *)acl_entry != end)
		goto fail;
	return acl;
fail:
	posix_acl_release(acl);
	return ERR_PTR(-EINVAL);
}
/**
 * 获取文件访问控制表内容
 * @inode为该文件inode
 * @type定义获取类型为ACL_TYPE_ACCESS或是ACL_TYPE_DEFAULT
 * 从而读取不同的内容
 * 成功时返回指向存储获取到内容的ACL表的指针
 */
struct posix_acl *hmfs_get_acl(struct inode *inode, int type)
{
	struct hmfs_acl_header *acl_header = NULL;
	struct posix_acl *acl;
	void *entry;
	int size;
	int ofs_access, ofs_default, ofs_end;

	acl_header = get_acl_block(inode);
	if (!acl_header)
		return ERR_PTR(-ENODATA);
	
	if (acl_header->a_version != cpu_to_le32(HMFS_ACL_VERSION))
		return ERR_PTR(-EINVAL);

	ofs_access = le16_to_cpu(acl_header->acl_access_ofs);
	ofs_default = le16_to_cpu(acl_header->acl_default_ofs);
	ofs_end = le16_to_cpu(acl_header->acl_end);
	if (type == ACL_TYPE_ACCESS) {
		if (acl_header->acl_access_ofs) {
			entry = JUMP(acl_header, le16_to_cpu(acl_header->acl_access_ofs));
			size = ofs_access < ofs_default ? ofs_default - ofs_access :
						ofs_end - ofs_access;
		} else
			return ERR_PTR(-ENODATA);
	} else if (type == ACL_TYPE_DEFAULT) {
		if (acl_header->acl_default_ofs) {
			entry = JUMP(acl_header, le16_to_cpu(acl_header->acl_default_ofs));
			size = ofs_access < ofs_default ? ofs_end - ofs_default :
						ofs_access - ofs_default;
		} else
			return ERR_PTR(-ENODATA);
	} else
		return ERR_PTR(-EINVAL);

	acl = hmfs_read_acl(entry, size);

	if (!IS_ERR(acl))
		set_cached_acl(inode, type, acl);

	return acl;
}
/**
 * 初始化@base_addr指向的acl块
 * 修改其h_magic值为HMFS_X_BLOCK_TAG_ACL
 * 修改a_version值为HMFS_ACL_VERSION
 */
static void init_acl_block(void *base_addr) 
{
	struct hmfs_acl_header *acl_header = ACL_HEADER(base_addr);

	XATTR_HDR(base_addr)->h_magic = cpu_to_le16(HMFS_X_BLOCK_TAG_ACL);
	acl_header->a_version = cpu_to_le32(HMFS_ACL_VERSION);
}	
/**
 * 写文件访问控制表
 * @inode为该文件的inode
 * @acl指向的结构体存储了要写入文件ACL的值
 * @size用于存储写入后的ACL大小
 * @type定义ACL类型为ACL_TYPE_ACCESS还是ACL_TYPE_DEFAULT
 * 成功则返回写入的访问控制表地址
 */
static void *hmfs_write_acl(struct inode *inode, const struct posix_acl *acl,
				size_t *size, int type)
{
	struct hmfs_acl_header *acl_header, *src_header;
	struct hmfs_acl_entry *entry;
	int i, ofs_access, ofs_default, ofs_end, cpy_size;
	
	src_header = get_acl_block(inode);
	
	acl_header = alloc_new_x_block(inode, HMFS_X_BLOCK_TAG_ACL, false);
	if (IS_ERR(acl_header))
		return ERR_PTR(-ENOSPC);

	init_acl_block(acl_header);
	entry = ACL_ENTRY(acl_header + 1);

	if (!src_header)
		goto write;
	
	ofs_access = le16_to_cpu(src_header->acl_access_ofs);
	ofs_default = le16_to_cpu(src_header->acl_default_ofs);
	ofs_end = le16_to_cpu(src_header->acl_end);
	if (type == ACL_TYPE_ACCESS) {
		if (ofs_default) {
			cpy_size = ofs_access < ofs_default ? ofs_end - ofs_default :
							ofs_access - ofs_default;
			hmfs_memcpy(entry, JUMP(src_header, ofs_default), cpy_size);
			acl_header->acl_default_ofs = cpu_to_le16(DISTANCE(acl_header, entry));
			entry = ACL_ENTRY(JUMP(entry, cpy_size));
		} else {
			acl_header->acl_default_ofs = 0;
		}
		acl_header->acl_access_ofs = cpu_to_le16(DISTANCE(acl_header, entry));
	}

	if (type == ACL_TYPE_DEFAULT) {
		if (ofs_access) {
			cpy_size = ofs_access < ofs_default ? ofs_default - ofs_access :
							ofs_end - ofs_access;
			hmfs_memcpy(entry, JUMP(src_header, ofs_access), cpy_size);
			acl_header->acl_access_ofs = cpu_to_le16(DISTANCE(acl_header, entry));
			entry = ACL_ENTRY(JUMP(entry, cpy_size));
		} else {
			acl_header->acl_access_ofs = 0;
		}
		acl_header->acl_default_ofs = cpu_to_le16(DISTANCE(acl_header, entry));
	}

write:
	for (i = 0; i < acl->a_count; i++) {
		entry->e_tag = cpu_to_le16(acl->a_entries[i].e_tag);
		entry->e_perm = cpu_to_le16(acl->a_entries[i].e_perm);

		switch (acl->a_entries[i].e_tag) {
		case ACL_USER:
			entry->e_id = cpu_to_le32(from_kuid(&init_user_ns, 
								acl->a_entries[i].e_uid));
			entry = ACL_ENTRY(JUMP(entry, ACL_ENTRY_SIZE));
			break;
		case ACL_GROUP:
			entry->e_id = cpu_to_le32(from_kgid(&init_user_ns,
								acl->a_entries[i].e_gid));
			entry = ACL_ENTRY(JUMP(entry, ACL_ENTRY_SIZE));
			break;
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			entry = ACL_ENTRY(JUMP(entry, ACL_SHORT_ENTRY_SIZE));
			break;
		default:
			goto fail;
		}
	}
	acl_header->acl_end = le16_to_cpu(DISTANCE(acl_header, entry));
	*size = hmfs_acl_size(acl->a_count);
	return (void *)acl_header;
fail:
	return ERR_PTR(-EINVAL);
}
/**
 * 设置文件访问控制表
 * @inode为该文件的inode
 * @acl指向的结构体存储了要写入文件ACL的值
 * @type定义ACL类型为ACL_TYPE_ACCESS还是ACL_TYPE_DEFAULT
 * 成功则返回0
 */
int hmfs_set_acl(struct inode *inode, struct posix_acl *acl, int type)
{
	struct hmfs_inode_info *fi = HMFS_I(inode);
	void *value = NULL;
	size_t size = 0;
	int error = 0;

	switch (type) {
	case ACL_TYPE_ACCESS:
		if (acl) {
			error = posix_acl_equiv_mode(acl, &inode->i_mode);
			if (error < 0)
				return error;
			set_acl_inode(fi, inode->i_mode);
			if (error == 0)
				acl = NULL;
		}
		break;

	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		break;

	default:
		return -EINVAL;
	}

	if (acl) {
		value = hmfs_write_acl(inode, acl, &size, type);
		if (IS_ERR(value)) {
			clear_inode_flag(fi, FI_ACL_MODE);
			return PTR_ERR(value);
		}
	}

	if (!error)
		set_cached_acl(inode, type, acl);

	clear_inode_flag(fi, FI_ACL_MODE);
	return error;
}
/**
 * 复制访问控制表
 * @acl为要复制的访问控制表
 * @flags为复制时使用的gfp掩码
 * 返回复制的访问控制表
 */
static struct posix_acl *hmfs_acl_clone(const struct posix_acl *acl,
				gfp_t flags)
{
	struct posix_acl *clone = NULL;
	int size;

	if (acl) {
		size = sizeof(struct posix_acl) + acl->a_count *
					sizeof(struct posix_acl_entry);
		clone = kmemdup(acl, size, flags);
		if (clone)
			atomic_set(&clone->a_refcount, 1);
	}
	return clone;
}
/**
 * 创建新节点时修改文件acl
 */
static int hmfs_acl_create_masq(struct posix_acl *acl, umode_t *mode_p)
{
	struct posix_acl_entry *pa, *pe;
	struct posix_acl_entry *group_obj = NULL, *mask_obj = NULL;
	umode_t mode = *mode_p;
	int not_equiv = 0;

	FOREACH_ACL_ENTRY(pa, acl, pe) {
		switch (pa->e_tag) {
		case ACL_USER_OBJ:
			pa->e_perm &= (mode >> 6) | ~S_IRWXO;
			mode &= (pa->e_perm << 6) | ~S_IRWXU;
			break;

		case ACL_USER:
		case ACL_GROUP:
			not_equiv = 1;
			break;

		case ACL_GROUP_OBJ:
			group_obj = pa;
			break;

		case ACL_OTHER:
			pa->e_perm &= mode | ~S_IRWXO;
			mode &= pa->e_perm | ~S_IRWXO;
			break;

		case ACL_MASK:
			mask_obj = pa;
			not_equiv = 1;
			break;

		default:
			return -EIO;
		}
	}

	if (mask_obj) {
		mask_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
		mode &= (mask_obj->e_perm << 3) | ~S_IRWXO;
	} else {
		if (!group_obj)
			return -EIO;
		group_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
		mode &= (group_obj->e_perm << 3) | ~S_IRWXO;
	}

	*mode_p = (*mode_p & ~S_IRWXUGO) | mode;
	return not_equiv;
}
/**
 * 创建一个访问控制表
 * @dir为参照的已有文件inode
 * @mode对应文件类型
 * @*default_acl指向dir文件的acl
 * @*acl指向新建的文件acl
 */
static int hmfs_acl_create(struct inode *dir, umode_t *mode,
				struct posix_acl **default_acl, struct posix_acl **acl)
{
	struct posix_acl *p;
	struct posix_acl *clone;
	int ret = 0;

	*acl = NULL;
	*default_acl = NULL;

	if (S_ISLNK(*mode) || !IS_POSIXACL(dir))
		return 0;

	p = hmfs_get_acl(dir, ACL_TYPE_DEFAULT);
	if (!p || p == ERR_PTR(-ENODATA) || p == ERR_PTR(-EOPNOTSUPP)) {
		*mode &= ~current_umask();
		return 0;
	}
	if (IS_ERR(p))
		return PTR_ERR(p);

	clone = hmfs_acl_clone(p, GFP_NOFS);
	if (!clone)
		goto no_mem;

	ret = hmfs_acl_create_masq(clone, mode);
	if (ret < 0)
		goto no_mem_clone;

	if (!ret)
		posix_acl_release(clone);
	else
		*acl = clone;

	if (!S_ISDIR(*mode))
		posix_acl_release(p);
	else
		*default_acl = p;

	return 0;
no_mem_clone:
	posix_acl_release(clone);
no_mem:
	posix_acl_release(p);
	return -ENOMEM;
}
/**
 * 初始化访问控制表
 * @inode为需要初始化的文件inode
 * @dir为参照的文件inode
 * 成功则返回0
 */
int hmfs_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl = NULL, *acl = NULL;
	int error = 0;

	error = hmfs_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (error)
		return error;

	if (default_acl) {
		error = hmfs_set_acl(inode, default_acl, ACL_TYPE_DEFAULT);
		posix_acl_release(default_acl);
	}

	if (acl) {
		if (!error)
			error = hmfs_set_acl(inode, acl, ACL_TYPE_ACCESS);
		posix_acl_release(acl);
	}

	return error;
}
/**
 * 列出文件POSIX_ACL_XATTR_ACCESS属性值
 * @dentry用于索引超级块
 * @list用于存储字符串
 * @list_size为list存储的最大空间
 * 返回属性值的字节长度
 */
size_t hmfs_acl_access_xattr_list(struct dentry *dentry, char *list, 
				size_t list_size, const char *name, size_t name_len, 
				int type)
{
	const size_t size = sizeof(POSIX_ACL_XATTR_ACCESS);
	struct hmfs_sb_info *sbi = HMFS_SB(dentry->d_sb);
	
	if (!test_opt(sbi, POSIX_ACL))
		return 0;
	if (list && size <= list_size)
		memcpy(list, POSIX_ACL_XATTR_ACCESS, size);
	return size;
}
/**
 * 列出文件POSIX_ACL_XATTR_DEFAULT属性值
 * @dentry用于索引超级块
 * @list用于存储字符串
 * @list_size为list存储的最大空间
 * 返回属性值的字节长度
 */
size_t hmfs_acl_default_xattr_list(struct dentry *dentry, char *list,
				size_t list_size, const char *name, size_t name_len,
				int type)
{
	const size_t size = sizeof(POSIX_ACL_XATTR_DEFAULT);
	struct hmfs_sb_info *sbi = HMFS_SB(dentry->d_sb);

	if (!test_opt(sbi, POSIX_ACL))
		return 0;
	if (list && size <= list_size)
		memcpy(list, POSIX_ACL_XATTR_DEFAULT, size);
	return size;
}
/**
 * 获取文件acl扩展属性
 * @dentry为该文件目录项
 * @buffer为属性复制到的目标缓冲区地址
 * @size为目标缓冲区最大大小
 * 成功则返回acl属性字节长度
 */
int hmfs_acl_xattr_get(struct dentry *dentry, const char *name, void *buffer,
				size_t size, int type)
{
	struct posix_acl *acl;
	int error;
	struct hmfs_sb_info *sbi = HMFS_SB(dentry->d_sb);

	if (strcmp(name, "") != 0)
		return -EINVAL;
	if (!test_opt(sbi, POSIX_ACL))
		return -EOPNOTSUPP;

	inode_read_lock(dentry->d_inode);
	acl = hmfs_get_acl(dentry->d_inode, type);
	inode_read_unlock(dentry->d_inode);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl == NULL)
		return -ENODATA;
	error = posix_acl_to_xattr(&init_user_ns, acl, buffer, size);
	posix_acl_release(acl);

	return error;
}
/**
 * 设置文件acl扩展属性
 * @dentry为该文件目录项
 * @value为要设置的属性值
 * @size为要设置的属性值字节数
 * 成功则返回0
 */
static int hmfs_acl_xattr_set(struct dentry *dentry, const char *name,
				const void *value, size_t size, int flags, int type)
{
	struct inode *inode = dentry->d_inode;
	struct posix_acl *acl;
	int error = 0, ilock;
	struct hmfs_sb_info *sbi = HMFS_SB(dentry->d_sb);

	if (strcmp(name, "") != 0)
		return -EINVAL;
	if (!test_opt(sbi, POSIX_ACL))
		return -EOPNOTSUPP;
	if (!inode_owner_or_capable(inode))
		return -EPERM;

	if (value) {
		acl = posix_acl_from_xattr(&init_user_ns, value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
		else if (acl) {
			error = posix_acl_valid(acl);
			if (error)
				goto release_and_out;
		}
	} else
		acl = NULL;

	ilock = mutex_lock_op(sbi);
	inode_write_lock(inode);
	error = hmfs_set_acl(inode, acl, type);
	inode_write_unlock(inode);
	mutex_unlock_op(sbi, ilock);

release_and_out:
	posix_acl_release(acl);
	return error;
}


const struct xattr_handler hmfs_acl_access_handler = {
	.prefix = POSIX_ACL_XATTR_ACCESS,
	.flags = ACL_TYPE_ACCESS,
	.list = hmfs_acl_access_xattr_list,
	.get = hmfs_acl_xattr_get,
	.set = hmfs_acl_xattr_set,
};

const struct xattr_handler hmfs_acl_default_handler = {
	.prefix = POSIX_ACL_XATTR_DEFAULT,
	.flags = ACL_TYPE_DEFAULT,
	.list = hmfs_acl_default_xattr_list,
	.get = hmfs_acl_xattr_get,
	.set = hmfs_acl_xattr_set,
};
