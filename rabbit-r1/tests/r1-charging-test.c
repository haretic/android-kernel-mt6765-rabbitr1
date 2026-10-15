// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define HZ 1000UL
#define min(a,b) ((a) < (b) ? (a) : (b))
#define min3(a,b,c) min(min(a,b),c)
#define clamp(a,b,c) min(((a) > (b) ? (a) : (b)),c)
#define msecs_to_jiffies(x) (x)
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define time_after_eq(a,b) (!time_before(a,b))
#define READ_ONCE(x) (x)
#define PSY_EVENT_PROP_CHANGED 1
#define NOTIFY_OK 0
#define dev_err_ratelimited(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; unsigned long delay; };
struct mutex { bool held; };
struct notifier_block { int unused; };
struct device { void *data; bool awake; };
struct device_attribute { int unused; };
typedef struct { int value; } atomic_t;
static int atomic_read(atomic_t *a) { return a->value; }
static void atomic_inc(atomic_t *a) { a->value++; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void pm_stay_awake(struct device *d) { assert(!d->awake); d->awake=true; }
static void pm_relax(struct device *d) { assert(d->awake); d->awake=false; }
static int kstrtoint(const char *s, int base, int *value)
{
    char *end; errno=0; long n=strtol(s,&end,base);
    if (end==s || errno || n<INT_MIN || n>INT_MAX || (*end && strcmp(end,"\n"))) return -EINVAL;
    *value=(int)n; return 0;
}
static unsigned long jiffies;
static void *system_highpri_wq;
static void mod_delayed_work(void *q, struct delayed_work *w, unsigned long delay) { w->delay=delay; }
static void queue_delayed_work(void *q, struct delayed_work *w, unsigned long delay) { w->delay=delay; }
static void cancel_delayed_work_sync(struct delayed_work *w) { w->delay=ULONG_MAX; }
enum { POWER_SUPPLY_HEALTH_GOOD, POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE };
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_DCP,
       POWER_SUPPLY_USB_TYPE_CDP, POWER_SUPPLY_USB_TYPE_C, POWER_SUPPLY_USB_TYPE_PD };
enum power_supply_property { POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_HEALTH,
       POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
       POWER_SUPPLY_PROP_USB_TYPE, POWER_SUPPLY_PROP_CURRENT_MAX, POWER_SUPPLY_PROP_ONLINE,
       POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
       POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,
       POWER_SUPPLY_PROP_PRECHARGE_CURRENT, POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
       POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, POWER_SUPPLY_PROP_CAPACITY, PROP_COUNT };
enum { POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO, POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE };
union power_supply_propval { int intval; };
struct power_supply { int values[PROP_COUNT]; };
static struct power_supply charger, battery, gadget, typec;
static unsigned int reads, writes, read_fail, write_fail, change_on_write;
static enum power_supply_property failed_read;
static atomic_t *generation_pointer;
static struct { enum power_supply_property prop; int value; } trace[100];
static int power_supply_get_property(struct power_supply *p, enum power_supply_property prop,
                                     union power_supply_propval *v)
{
    if (++reads == read_fail) { failed_read=prop; return -EIO; }
    v->intval=p->values[prop]; return 0;
}
static int power_supply_set_property(struct power_supply *p, enum power_supply_property prop,
                                     const union power_supply_propval *v)
{
    assert(writes < 100);
    trace[writes].prop=prop; trace[writes].value=v->intval;
    if (++writes == change_on_write) atomic_inc(generation_pointer);
    p->values[prop]=v->intval; // Model a failing write which still reaches hardware.
    return writes == write_fail ? -EIO : 0;
}
#include "policy-under-test.h"
static struct r1_charge_policy p;
static struct device dev={.data=&p};
static struct r1_charge_sample good(void)
{
    return (struct r1_charge_sample){.valid=true, .present=1, .plugged=1,
        .health=POWER_SUPPLY_HEALTH_GOOD, .temperature=250, .voltage=3800000,
        .usb_type=POWER_SUPPLY_USB_TYPE_DCP, .gadget_ua=0, .capacity=50};
}
static void reset(void)
{
    memset(&p,0,sizeof(p)); dev.awake=false; memset(&charger,0,sizeof(charger)); memset(&battery,0,sizeof(battery));
    memset(&gadget,0,sizeof(gadget)); memset(&typec,0,sizeof(typec));
    reads=writes=read_fail=write_fail=change_on_write=0; jiffies=100;
    p.dev=&dev; p.charger=&charger; p.battery=&battery; p.gadget=&gadget;
    p.requested_ua=1000000; p.charge_stop_level=100; generation_pointer=&p.generation;
    charger.values[POWER_SUPPLY_PROP_PRESENT]=1;
    charger.values[POWER_SUPPLY_PROP_HEALTH]=POWER_SUPPLY_HEALTH_GOOD;
    charger.values[POWER_SUPPLY_PROP_USB_TYPE]=POWER_SUPPLY_USB_TYPE_DCP;
    battery.values[POWER_SUPPLY_PROP_PRESENT]=1;
    battery.values[POWER_SUPPLY_PROP_TEMP]=250;
    battery.values[POWER_SUPPLY_PROP_VOLTAGE_NOW]=3800000;
    battery.values[POWER_SUPPLY_PROP_CAPACITY]=50;
}
static void decisions(void)
{
    unsigned int cases=0;
    reset();
    for (int ua=-100000; ua<=1100000; ua+=50000)
        assert(r1_charge_limit_valid(ua) == (ua>=500000 && ua<=1000000 && ua%100000==0));
    for (int cap=500000; cap<=1000000; cap+=100000)
    for (int temp=-400; temp<=650; temp+=5)
    for (int voltage=2900000; voltage<=4500000; voltage+=10000)
    for (int usb=0; usb<=3; usb++) {
        struct r1_charge_sample s=good();
        p.cold=p.hot=p.full=false; p.requested_ua=cap;
        s.temperature=temp; s.voltage=voltage; s.usb_type=usb; s.gadget_ua=500000;
        struct r1_charge_target t=r1_charge_decide(&p,&s);
        assert(t.charge_ua<=cap && t.charge_ua<=1000000 && t.input_ua<=1000000);
        assert(t.charge_ua==0 || (t.charge_ua>=500000 && t.charge_ua%100000==0));
        int target=temp<150 ? 4200000 : temp>=430 ? 4100000 : 4400000;
        if (temp<50 || temp>=500 || voltage<3000000 || voltage>=target)
            assert(!t.charge_ua);
        if (t.charge_ua) assert(t.voltage_uv==target);
        if (temp<150 || temp>=430 || usb==POWER_SUPPLY_USB_TYPE_SDP || !usb) assert(t.charge_ua<=500000);
        if (temp>=150 && temp<430 && voltage>=3000000 && voltage<4400000 && usb>=2)
            assert(t.charge_ua==cap);
        cases++;
    }
    reset(); struct r1_charge_sample s=good();
    s.usb_type=POWER_SUPPLY_USB_TYPE_SDP;
    const int budgets[]={-1,0,2000,8000,100000,499999,500000};
    for (unsigned int i=0;i<sizeof(budgets)/sizeof(budgets[0]);i++) {
        s.gadget_ua=budgets[i]; struct r1_charge_target t=r1_charge_decide(&p,&s);
        assert(t.charge_ua==(budgets[i]>=500000 ? 500000 : 0));
        if (!t.charge_ua) assert(!t.input_ua);
    }
    s=good(); s.typec_online=1; s.typec_type=POWER_SUPPLY_USB_TYPE_PD;
    s.typec_ua=3000000; s.typec_uv=5000000;
    assert(r1_charge_decide(&p,&s).charge_ua==1000000);
    s.typec_uv=9000000; assert(!r1_charge_decide(&p,&s).charge_ua);
    s=good(); s.temperature=49; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.temperature=69; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.temperature=70; assert(r1_charge_decide(&p,&s).charge_ua==500000);
    s.temperature=500; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.temperature=480; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.temperature=479; assert(r1_charge_decide(&p,&s).charge_ua==500000);
    s=good(); s.voltage=4400000; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.voltage=4300000; assert(!r1_charge_decide(&p,&s).charge_ua);
    s.voltage=4299999; assert(r1_charge_decide(&p,&s).charge_ua==1000000);
    assert(r1_charge_decide(&p,&s).voltage_uv==4400000);
    s=good(); s.health=POWER_SUPPLY_HEALTH_UNSPEC_FAILURE; assert(!r1_charge_decide(&p,&s).charge_ua);
    s=good(); s.timer_expired=true; assert(r1_charge_decide(&p,&s).reason==R1_TIMER);
    s=good(); s.valid=false; assert(!r1_charge_decide(&p,&s).charge_ua);
    s=good(); s.present=0; assert(!r1_charge_decide(&p,&s).charge_ua);
    printf("PASS: %u decision combinations, boundaries, USB budgets and hysteresis\n",cases);
}
static void levels(void)
{
    struct r1_charge_sample s; struct r1_charge_target t; unsigned int cases=0;
    for (int start=0; start<=100; start+=5)
    for (int stop=0; stop<=100; stop+=5) {
        reset(); p.charge_start_level=start; p.charge_stop_level=stop;
        bool held=false;
        /* Charge up, then drain, crossing both levels and an unknown level. */
        for (int step=0; step<=300; step++) {
            int cap = step<=100 ? step : step<=200 ? 200-step : (step%7 ? step-200 : -1);
            s=good(); s.capacity=cap; t=r1_charge_decide(&p,&s);
            if (stop>=100) held=false;
            else if (cap<0) ; /* unknown keeps the decision */
            else if (cap>=stop) held=true;
            else if (cap<=start) held=false;
            assert((t.reason==R1_LEVEL_LIMIT)==held);
            assert(held ? !t.charge_ua && t.input_ua==1000000 : t.charge_ua==1000000);
            cases++;
        }
    }
    /* Before any known level, an unknown one charges. */
    reset(); p.charge_stop_level=50; s=good(); s.capacity=-1;
    assert(r1_charge_decide(&p,&s).charge_ua==1000000);
    reset(); p.charge_stop_level=50; s=good(); s.capacity=60; s.plugged=0;
    assert(r1_charge_decide(&p,&s).reason==R1_UNPLUGGED);
    s=good(); s.capacity=60; s.temperature=10; assert(r1_charge_decide(&p,&s).reason==R1_TEMPERATURE);
    printf("PASS: %u charge level limit steps\n", cases);
}
static void transactions(void)
{
    reset(); r1_charge_work(&p.work.work); unsigned int total=writes;
    assert(total==8 && p.applied.charge_ua==1000000 && p.work.delay==2*HZ);
    assert(trace[0].prop==POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR && trace[0].value==1);
    assert(trace[7].prop==POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR && trace[7].value==0);
    for (unsigned int fail=1;fail<=total;fail++) {
        reset(); write_fail=fail; r1_charge_work(&p.work.work);
        assert(p.error==-EIO && !p.applied.charge_ua);
        assert(charger.values[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT]==0);
        assert(charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==1);
    }
    reset(); r1_charge_work(&p.work.work); total=reads;
    for (unsigned int fail=1;fail<=total;fail++) {
        reset(); read_fail=fail; r1_charge_work(&p.work.work);
        if (failed_read==POWER_SUPPLY_PROP_CAPACITY && fail>3) {
            /* Capacity is optional: an unknown level charges normally. */
            assert(!p.error && p.applied.charge_ua==1000000);
            continue;
        }
        assert(p.error==-EIO && !p.applied.charge_ua && writes==2);
        assert(charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==1);
    }
    for (unsigned int change=1;change<=8;change++) {
        reset(); change_on_write=change; r1_charge_work(&p.work.work);
        assert(p.error==-EAGAIN && !p.applied.charge_ua);
        assert(charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==1);
    }
    reset(); r1_charge_work(&p.work.work); writes=0;
    r1_charge_work(&p.work.work); assert(writes==1 && trace[0].value==0); // Renew lease.
    charger.values[POWER_SUPPLY_PROP_HEALTH]=POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
    r1_charge_work(&p.work.work); assert(p.applied.reason==R1_TIMER);
    charger.values[POWER_SUPPLY_PROP_HEALTH]=POWER_SUPPLY_HEALTH_GOOD;
    r1_charge_work(&p.work.work); assert(p.applied.reason==R1_TIMER);
    charger.values[POWER_SUPPLY_PROP_PRESENT]=0; r1_charge_work(&p.work.work);
    charger.values[POWER_SUPPLY_PROP_PRESENT]=1; r1_charge_work(&p.work.work);
    assert(p.applied.charge_ua==1000000);
    jiffies+=R1_SESSION_TIMEOUT; r1_charge_work(&p.work.work); assert(p.applied.reason==R1_TIMER);
    reset(); p.typec=&typec; r1_charge_work(&p.work.work); assert(!p.applied.charge_ua);
    reset(); r1_charge_work(&p.work.work); assert(dev.awake && p.awake);
    read_fail=reads+1; r1_charge_work(&p.work.work); assert(dev.awake);
    charger.values[POWER_SUPPLY_PROP_PRESENT]=0; r1_charge_work(&p.work.work); assert(!dev.awake);
    read_fail=reads+1; r1_charge_work(&p.work.work); assert(!dev.awake);
    charger.values[POWER_SUPPLY_PROP_PRESENT]=1; r1_charge_work(&p.work.work); assert(dev.awake);
    reset(); p.typec=&typec; r1_charge_work(&p.work.work); assert(!dev.awake);
    reset(); r1_charge_work(&p.work.work); assert(!r1_charge_suspend(&dev));
    assert(p.suspended && !p.applied.charge_ua && charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==1);
    assert(!r1_charge_resume(&dev) && !p.suspended && p.work.delay==0 && dev.awake);
    charger.values[POWER_SUPPLY_PROP_PRESENT]=0; assert(!r1_charge_suspend(&dev) && dev.awake);
    assert(!r1_charge_resume(&dev) && dev.awake);
    r1_charge_work(&p.work.work); assert(!dev.awake);
    reset(); r1_charge_work(&p.work.work); write_fail=writes+1;
    assert(r1_charge_suspend(&dev)==-EIO && !p.suspended && p.work.delay==0);
    reset(); assert(charge_control_limit_store(&dev,NULL,"600000\n",7)==7 && p.requested_ua==600000);
    assert(charge_control_limit_store(&dev,NULL,"0",1)==-ERANGE);
    assert(charge_control_limit_store(&dev,NULL,"500001",6)==-ERANGE);
    assert(charge_control_limit_store(&dev,NULL,"500000x",7)==-EINVAL);
    p.stopping=true; assert(charge_control_limit_store(&dev,NULL,"500000",6)==-ESHUTDOWN);
    reset(); r1_charge_notify(&p.notifier,PSY_EVENT_PROP_CHANGED,&gadget);
    assert(p.generation.value==1 && p.work.delay==0);
    r1_charge_notify(&p.notifier,PSY_EVENT_PROP_CHANGED,&charger); assert(p.generation.value==1);
    reset(); assert(r1_level_store(&p,&p.charge_stop_level,"80\n",3)==3 && p.charge_stop_level==80);
    assert(r1_level_store(&p,&p.charge_start_level,"70",2)==2 && p.charge_start_level==70);
    assert(r1_level_store(&p,&p.charge_stop_level,"101",3)==-ERANGE);
    assert(r1_level_store(&p,&p.charge_start_level,"-1",2)==-ERANGE);
    assert(r1_level_store(&p,&p.charge_stop_level,"8x",2)==-EINVAL && p.charge_stop_level==80);
    battery.values[POWER_SUPPLY_PROP_CAPACITY]=80; r1_charge_work(&p.work.work);
    assert(!p.error && p.applied.reason==R1_LEVEL_LIMIT && !p.applied.charge_ua);
    /* Held at the limit, the input still powers the system with charging inhibited. */
    assert(p.applied.input_ua==1000000 && charger.values[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT]==1000000);
    assert(charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==1);
    battery.values[POWER_SUPPLY_PROP_CAPACITY]=71; r1_charge_work(&p.work.work);
    assert(p.applied.reason==R1_LEVEL_LIMIT && !p.applied.charge_ua);
    battery.values[POWER_SUPPLY_PROP_CAPACITY]=70; r1_charge_work(&p.work.work);
    assert(p.applied.charge_ua==1000000 && charger.values[POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR]==0);
    p.stopping=true; assert(r1_level_store(&p,&p.charge_stop_level,"50",2)==-ESHUTDOWN); p.stopping=false;
    puts("PASS: read/write faults, superseded grants, safety timeout, suspend and sysfs validation");
}
int main(void) { decisions(); levels(); transactions(); return 0; }
