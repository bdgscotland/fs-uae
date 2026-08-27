#ifndef FS_UAE_CMDFIFO_H
#define FS_UAE_CMDFIFO_H

/* Scripted input via a named pipe. Enabled by setting the environment
 * variable FSUAE_CMD_FIFO (or the cmd_fifo option) to a path. Allows an
 * external process to drive the emulated machine without the emulator
 * window needing host input focus. */

void fs_uae_init_cmd_fifo(void);

/* Called once per emulated frame from the input handler, on the emulation
 * thread, to queue any scheduled button/key releases which have come due. */
void fs_uae_cmd_fifo_update(int frame);

#endif /* FS_UAE_CMDFIFO_H */
