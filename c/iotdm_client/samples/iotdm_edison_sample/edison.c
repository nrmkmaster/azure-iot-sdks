

// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "iotdm_internal.h"

pid_t child_download = -1;
pid_t child_update = -1;
pid_t child_factory_reset = -1;

void _system(const char *command)
{
    LogInfo("running [%s]\r\n", command);
    int i = system(command);
    LogInfo("Command [%s] returned %d\r\n", command, i);
}

bool is_process_running(pid_t *p)
{
    if (*p <= 0)
    {
        return false;
    }
    else
    {
        int status;
        pid_t result = waitpid(*p, &status, WNOHANG);
        if (result == 0)
        {
            return true;
        }
        else
        {
            *p = -1;
            return false;
        }
    }
}

bool is_download_happening()
{
    return is_process_running(&child_download);
}

bool is_update_happening()
{
    return is_process_running(&child_update);
}

bool is_factory_reset_happening()
{
    return is_process_running(&child_factory_reset);
}

bool is_any_child_process_running()
{
    bool ret = false;
    if (is_download_happening())
    {
        LogInfo("download is in progress\r\n");
        ret = true;
    }
    if (is_update_happening())
    {
        LogInfo("update is in progress\r\n");
        ret = true;
    }
    if (is_factory_reset_happening())
    {
        LogInfo("factory reset is in progress\r\n");
        ret = true;
    }
}

char *get_serial_number()
{
    char *sn = NULL;
    FILE *fd = fopen("/factory/serial_number", "r");

    if (NULL == fd)
    {
        sn = lwm2m_strdup("");
    }
    else
    {
        fseek(fd, 0, SEEK_END);
        size_t size = ftell(fd);
        sn = (char *)malloc(size * sizeof(char));
        if (NULL != sn)
        {
            fseek(fd, 0L, SEEK_SET);
            int count = fread(sn, sizeof(char), size, fd);
            sn[count - 1] = '\0';
        }
        fclose(fd);
    }

    return sn;
}

char *get_firmware_version()
{
    char *version = NULL;
    FILE *fd = fopen("/etc/version", "r");

    if (NULL == fd)
    {
        version = lwm2m_strdup("");
    }
    else
    {
        fseek(fd, 0, SEEK_END);
        size_t size = ftell(fd);
        version = (char *)malloc(size * sizeof(char));
        if (NULL != version)
        {
            fseek(fd, 0L, SEEK_SET);
            int count = fread(version, sizeof(char), size, fd);
            fclose(fd);
            version[count - 1] = '\0';

            char *atSign = strchr(version, '@');
            if (atSign != NULL) *atSign = '\0';
       }

        close(fd);
    }
    return version;
}

bool spawn_reboot_process()
{
    bool ret = false;
    LogInfo("\n\t REBOOT\r\n\n");

    // if the server tells us to reboot, we reboot. This protects us against
    // child processes that are hung.
    if (is_any_child_process_running() == true)
    {
        LogInfo("Reboot requested while another process is running.  Continuing\r\n");
    }

    pid_t child = fork();
    if (child == 0)
    {
        ThreadAPI_Sleep(500);

        setuid(0);
        sync();

        LogInfo("\n\t REBOOT - fork() \r\n\n");
        reboot(RB_AUTOBOOT);
    }
    else if (child > 0)
    {
        ret = true;
    }
    else
    {
        LogError("fork error - %d\r\n",child);
    }

    return ret;
}

bool spawn_factoryreset_process()
{
    bool ret = false;
    LogInfo("\n\t FACTORY RESET\r\n\n");

    if (is_any_child_process_running() == true)
    {
        LogError("Cannot factory reset while another process is running\r\n");
    }
    else
    {
        pid_t child = fork();

        if (child == 0)
        {
            LogInfo("** Reset To Factory started\r\n");

            _system("/usr/bin/wget https://downloadmirror.intel.com/25028/eng/edison-image-ww25.5-15.zip -O /home/root/factory.zip");
            ThreadAPI_Sleep(1500);

            setuid(0);

            _system("mkdir -p /update");
            _system("systemctl stop clloader");
            _system("echo on > /sys/devices/pci0000:00/0000:00:11.0/dwc3-device.1/power/control");
            _system("rmmod g_multi");
            _system("losetup -o 8192 /dev/loop0 /dev/disk/by-partlabel/update");
            _system("mount /dev/loop0 /update");
            _system("rm -rf /update/* /update/.[!.]* /update/..?*");
            _system("unzip -o /home/root/factory.zip -d /update");

            ThreadAPI_Sleep(1500);
            unlink("/home/root/factory.zip");

            _system("reboot ota");
        }
        else if (child > 0)
        {
            child_factory_reset = child;
            ret = true;
        }
        else
        {
            child_factory_reset = -1;
            LogError("fork error - %d\r\n", child);
        }
    }
    return ret;
}

bool spawn_download_process(const char *uri)
{
    bool ret = false;
    LogInfo("Downloading [%s]\r\n", uri);

    if (is_any_child_process_running() == true)
    {
        LogError("Cannot download while another process is running\r\n");
    }
    else
    {
        unlink("/home/root/newFirmware.zip");

        pid_t child = fork();
        if (child == 0)
        {
            char buffer[1024];

#if 0
            sprintf(buffer, "/usr/bin/wget \"%s\" -O /home/root/nf.zip", STRING_c_str(value));
            _system(buffer);
#endif

            sprintf(buffer, "cp /home/root/nf.zip /home/root/newFirmware.zip");
            _system(buffer);
            LogInfo("** Download complete\r\n");
            exit(0);
        }
        else if (child > 0)
        {
            child_download = child;
            ret = true;
        }
        else
        {
            child_download = -1;
            LogError("fork error - %d\r\n", child);
        }
    }
    return ret;
}

bool spawn_update_process()
{
    bool ret = false;
    LogInfo("\n\t FIRMWARE UPDATE\r\n\n");

    if (is_any_child_process_running() == true)
    {
        LogError("Cannot factory reset while another process is running\r\n");
    }
    else
    {
        pid_t child = fork();

        if (child == 0)
        {
            LogInfo("** Update started\r\n");

            // Give the client time to repsond to the service.  
            ThreadAPI_Sleep(1500);

            setuid(0);

            _system("mkdir -p /update");
            _system("systemctl stop clloader");
            _system("echo on > /sys/devices/pci0000:00/0000:00:11.0/dwc3-device.1/power/control");
            _system("rmmod g_multi");
            _system("losetup -o 8192 /dev/loop0 /dev/disk/by-partlabel/update");
            _system("mount /dev/loop0 /update");
            _system("rm -rf /update/* /update/.[!.]* /update/..?*");
            _system("unzip -o /home/root/newFirmware.zip -d /update");

            ThreadAPI_Sleep(1500);
            unlink("/home/root/newFirmware.zip");

            LogInfo("** Update complete\r\n");
            _system("reboot ota");
        }
        else if (child > 0)
        {
            child_update = child;
            ret = true;
        }
        else
        {
            child_update = -1;
            LogError("fork error - %d\r\n ", child);
        }
    }
    return ret;
}

