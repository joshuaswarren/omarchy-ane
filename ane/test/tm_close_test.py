#!/usr/bin/env python3
"""Exercise actual DRM release and postclose with a failed power resume."""
import pathlib
import subprocess
import tempfile

source = (pathlib.Path(__file__).resolve().parents[1] / 'src/ane_drv.c').read_text()
post = source[source.index('static void ane_drm_postclose('):source.index('\n/*\n * The driver only owns', source.index('static void ane_drm_postclose('))]
release = source[source.index('static int ane_drm_release('):source.index('\nstatic int ane_drm_mmap(', source.index('static int ane_drm_release('))]
prefix = r'''
#include <stdbool.h>
#include <stdio.h>
struct device { int unused; };
struct ane_device { int engine_lock; bool removed; struct device *dev; };
struct drm_device { void *dev_private; struct device *dev; };
struct drm_minor { struct drm_device *dev; };
struct drm_file { struct drm_minor *minor; };
struct file { void *private_data; };
struct inode { int unused; };
static int recoveries, resume_result, gets, pm_puts, releases;
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
static int ane_tm_recover(struct ane_device *a) { (void)a; recoveries++; return 0; }
static struct device *get_device(struct device *d) { return d; }
static void put_device(struct device *d) { (void)d; }
static int pm_runtime_resume_and_get(struct device *d) { (void)d; gets++; return resume_result; }
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void pm_runtime_put_autosuspend(struct device *d) { (void)d; pm_puts++; }
'''
suffix = r'''
int main(void) {
    struct device dev = {0}; struct ane_device ane = { .dev = &dev };
    struct drm_device drm = { .dev_private = &ane, .dev = &dev };
    struct drm_minor minor = { .dev = &drm }; struct drm_file priv = { .minor = &minor };
    struct file file = { .private_data = &priv }; struct inode inode = {0};
    resume_result = -1;
    int rc = ane_drm_release(&inode, &file);
    if (rc != 0 || recoveries || pm_puts || gets != 2 || releases != 1) {
        fprintf(stderr, "failed resume: rc=%d recoveries=%d gets=%d puts=%d releases=%d\n",
                rc, recoveries, gets, pm_puts, releases); return 1;
    }
    resume_result = 0; gets = pm_puts = releases = 0;
    rc = ane_drm_release(&inode, &file);
    if (rc != 0 || recoveries != 1 || gets != 2 || pm_puts != 2 || releases != 1) {
        fprintf(stderr, "successful resume: rc=%d recoveries=%d gets=%d puts=%d releases=%d\n",
                rc, recoveries, gets, pm_puts, releases); return 1;
    }
    printf("release power accounting and postclose: PASS\n"); return 0;
}
'''
bridge = 'static int drm_release(struct inode *i, struct file *f) { (void)i; struct drm_file *p = f->private_data; releases++; ane_drm_postclose(p->minor->dev, p); return 0; }\n'
with tempfile.TemporaryDirectory() as tmp:
    binary = pathlib.Path(tmp) / 'close-test'
    subprocess.run(['cc', '-std=gnu11', '-x', 'c', '-o', str(binary), '-'],
                   input=prefix + post + bridge + release + suffix, text=True, check=True)
    subprocess.run([str(binary)], check=True)
