/*
 * Inode table btree leaf operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "hexdump.c"
#include "tux3.h"

struct ileaf { u16 magic, count; inum_t inum; char table[]; };

/*
 * inode leaf format
 *
 * A leaf has a small header followed by a table of extents.  A vector of
 * offsets within the block grows down from the top of the leaf towards the
 * top of the extent table, indexed by the difference between inum and
 * leaf->inum, the base inum of the table block.
 */

int ileaf_init(SB, void *leaf)
{
	printf("initialize inode leaf %p\n", leaf);
	*(struct ileaf *)leaf = (struct ileaf){ 0x90de };
	return 0;
}

struct ileaf *ileaf_create(SB)
{
	struct ileaf *ileaf = malloc(sb->blocksize);
	ileaf_init(sb, ileaf);
	return ileaf;
}

int ileaf_sniff(SB, void *leaf)
{
	return ((struct ileaf *)leaf)->magic == 0x90de;
}

void ileaf_destroy(SB, struct ileaf *leaf)
{
	assert(ileaf_sniff(sb, leaf));
	free(leaf);
}

unsigned ileaf_used(SB, struct ileaf *leaf)
{
	u16 *dict = (void *)leaf + sb->blocksize, *base = dict - leaf->count;
	return (void *)dict - (void *)base + base == dict ? 0 : *base ;
}

unsigned ileaf_free(SB, struct ileaf *leaf)
{
	return sb->blocksize - ileaf_used(sb, leaf) - sizeof(struct ileaf);
}

void ileaf_dump(SB, struct ileaf *leaf)
{
	u16 *dict = (void *)leaf + sb->blocksize, offset = 0, inum = leaf->inum;
	printf("%i inodes, %i free:\n", leaf->count, ileaf_free(sb, leaf));
	//hexdump(dict - leaf->count, leaf->count * 2);
	for (int i = -1; i >= -leaf->count; i--, inum++) {
		int limit = dict[i], size = limit - offset;
		printf("  %i: ", inum);
		//printf("[%i] ", offset);
		if (size < 0)
			printf("<corrupt>\n");
		else if (size == 0)
			printf("<empty>\n");
		else
			hexdump(leaf->table + offset, size);
		offset = limit;
	}
}

void *ileaf_lookup(SB, struct ileaf *leaf, inum_t inum, unsigned *size)
{
	assert(inum > leaf->inum);
	inum_t at = inum - leaf->inum;
	assert(at < 999); // !!! calculate this properly: max inode possible with max dict
	printf("lookup inode %Lx, %Lx + %Lx\n", inum, leaf->inum, at);
	u16 *dict = (void *)leaf + sb->blocksize, offset = (at ? *(dict - at) : 0);
	return (*size = *(dict - at - 1) - offset) ? leaf->table + offset : NULL;
}

int ileaf_check(SB, struct ileaf *leaf)
{
	char *why;
	why = "not an inode table leaf";
	if (leaf->magic != 0x90de);
		goto eek;
	return 0;
eek:
	printf("%s!\n", why);
	return -1;
}

void ileaf_trim(SB, struct ileaf *leaf) {
	u16 *dict = (void *)leaf + sb->blocksize;
	while (leaf->count > 1 && *(dict - leaf->count) == *(dict - leaf->count + 1))
		leaf->count--;
	if (leaf->count == 1 && !*(dict - 1))
		leaf->count = 0;
}

tuxkey_t ileaf_split(SB, void *base, void *base2, int fudge)
{
	assert(ileaf_sniff(sb, base));
	struct ileaf *leaf = base, *dest = base2;
	u16 *dict = (void *)leaf + sb->blocksize, *destdict = (void *)dest + sb->blocksize;

	/* binsearch inum nearest middle */
	unsigned at = 1, hi = leaf->count;
	while (at < hi) {
		int mid = (at + hi) / 2;
		if (*(dict - mid) < (sb->blocksize / 2) + fudge)
			at = mid + 1;
		else
			hi = mid;
	}
	printf("split at %i\n", (sb->blocksize / 2) + fudge);

	/* should trim leading empty inodes on copy */
	unsigned split = *(dict - at), free = *(dict - leaf->count);
	printf("copy out %i bytes at %i\n", free - split, split);
	assert(free >= split);
	memcpy(dest->table, leaf->table + split, free - split);
	dest->count = leaf->count - at;
	veccopy(destdict - dest->count, dict - leaf->count, dest->count);
	for (int i = 1; i <= dest->count; i++)
		*(destdict - i) -= *(dict - at);
	dest->inum = leaf->inum + at;
	leaf->count = at;
	memset(leaf->table + split, 0, (char *)(dict - leaf->count) - (leaf->table + split));
	ileaf_trim(sb, leaf);
	return dest->inum;
}

void ileaf_merge(SB, struct ileaf *leaf, struct ileaf *from)
{
	if (!from->count)
		return;
	u16 *dict = (void *)leaf + sb->blocksize, *fromdict = (void *)from + sb->blocksize;
	unsigned at = leaf->count, free = at ? *(dict - at) : 0;
	unsigned size = from->count ? *(fromdict - from->count) : 0;
	printf("copy in %i bytes %i %i\n", size, at + 1, at + from->count);
	memcpy(leaf->table + free, from->table, size);
	veccopy(dict - (leaf->count += from->count), fromdict - from->count, from->count);
	for (int i = at + 1; at && i <= at + from->count; i++)
		*(dict - i) += *(dict - at);
}

void *ileaf_expand(SB, void *base, inum_t inum, unsigned more)
{
	assert(ileaf_sniff(sb, base));
	struct ileaf *leaf = base;
	assert(inum > leaf->inum);
	u16 *dict = (void *)leaf + sb->blocksize;
	unsigned at = inum - leaf->inum;

	/* extend with empty inodes */
	while (leaf->count <= at) {
		*(dict - leaf->count - 1) = leaf->count ? *(dict - leaf->count) : 0;
		leaf->count++;
	}

	u16 free = *(dict - leaf->count);
	unsigned offset = at ? *(dict - at) : 0, size = *(dict - at - 1) - offset;
	void *inode = leaf->table + offset;
	printf("expand inum %u at %i/%i by %i\n", at, offset, size, more);
	for (int i = at + 1; i <= leaf->count; i++)
		*(dict - i) += more;
	memmove(inode + size + more, inode + size, free - offset);
	return inode + size;
}

void *inode_append(SB, struct ileaf *leaf, inum_t inum, unsigned more, char fill)
{
	char *where = ileaf_expand(sb, leaf, inum, more);
	memset(where, fill, more);
}

void ileaf_test(SB)
{
	printf("--- test inode table leaf methods ---\n");
	struct ileaf *leaf = ileaf_create(sb);
	struct ileaf *dest = ileaf_create(sb);
	ileaf_dump(sb, leaf);
	inode_append(sb, leaf, 3, 2, 'a');
	inode_append(sb, leaf, 4, 4, 'b');
	inode_append(sb, leaf, 6, 6, 'c');
	ileaf_dump(sb, leaf);
	ileaf_split(sb, leaf, dest, -(sb->blocksize / 2));
	ileaf_dump(sb, leaf);
	ileaf_dump(sb, dest);
	ileaf_merge(sb, leaf, dest);
	ileaf_dump(sb, leaf);
	inode_append(sb, leaf, 3, 3, 'x');
	ileaf_dump(sb, leaf);
	inode_append(sb, leaf, 8, 3, 'y');
	ileaf_dump(sb, leaf);
	unsigned size = 0;
	char *inode = ileaf_lookup(sb, leaf, 3, &size);
	hexdump(inode, size);
	ileaf_destroy(sb, leaf);
	ileaf_destroy(sb, dest);
}

int main(int argc, char *argv[])
{
	ileaf_test(&(struct sb){ .blocksize = 4096 });
	return 0;
}