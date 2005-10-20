/*---------------------------------------------------------------------------
 * Gamedriver Callout handling.
 *
 *---------------------------------------------------------------------------
 * Callouts are delayed calls to (non-static) functions, with delays
 * measured in seconds. The minimal resolution is of course the duration
 * a backend cycle. The command_giver is saved over the delay.
 *
 * As a simplistic measure against 'rabbits' (think fork-bombs using
 * callouts), the callouts of one user can use only MAX_EVAL_COST at
 * one time altogether.
 *
 * Pending call outs are held in a list, sorted in ascending order of
 * remaining delaytime. The actual delay values stored are deltas: the
 * delay this callout is to be scheduled after its predecessor in
 * the list.
 *
 * A second list holds freed call structures to reduce memory management
 * overhead.
 *
 * TODO: The CHUNKed allocation would be nice to have as generic module.
 * TODO: It would be nice if the callout would store from where the
 * TODO:: callout originated and fake a control-stack entry for a proper
 * TODO:: traceback. However, this has to take swapping into account.
 *---------------------------------------------------------------------------
 */

#include "driver.h"
#include "typedefs.h"

#include "call_out.h"
#include "actions.h"
#include "array.h"
#include "backend.h"
#include "closure.h"
#include "comm.h"
#include "exec.h"
#include "gcollect.h"
#include "interpret.h"
#include "main.h"
#include "object.h"
#include "simulate.h"
#include "stralloc.h"
#include "strfuns.h"
#include "svalue.h"
#include "swap.h"
#include "wiz_list.h"
#include "xalloc.h"

#include "../mudlib/sys/debug_info.h"
 
/*-------------------------------------------------------------------------*/

  /* The description of one callout.
   *
   * The function to call be either given by object:name or as closure.
   */

struct call {
    /* TODO: p_int or mp_int */ int delta;           /* Delay in relation to the previous structure */
    callback_t fun;
    struct call *next;   /* link to next structure */
    object_t *command_giver;  /* the saved command_giver */
};


#define CHUNK_SIZE 20
  /* Structures are allocated in chunks of this size, to reduce the
   * malloc overhead.
   */

static struct call *call_list = NULL;
  /* The list of pending call_outs, sorted in ascending order of delay.
   */

static struct call *call_list_free = NULL;
  /* The list of unused call structures.
   */

static long num_call = 0;
  /* Number of allocated callouts.
   */

static object_t call_out_nil_object;
  /* Callouts with no argument let call.v use this variable as
   * placeholder.
   */

/*-------------------------------------------------------------------------*/
static INLINE void
free_call (struct call *cop)

/* Deallocate all resources bound to <cop> and put <cop> into the free list.
 * This can be used for used and unused callouts alike.
 */

{
    free_callback(&(cop->fun));

    if (cop->command_giver)
        free_object(cop->command_giver, "free_call");

    cop->next = call_list_free;
    call_list_free = cop;
} /* free_call() */

/*-------------------------------------------------------------------------*/
svalue_t *
new_call_out (svalue_t *sp, short num_arg)

/* EFUN: call_out()
 *
 *   void call_out(string fun, int delay, mixed arg, ...)
 *   void call_out(closure cl, int delay, mixed arg, ...)
 *
 * Set up a call to function fun or closure cl in the current
 * object. The call will take place in delay seconds, with the
 * remaining argument list provided. References in the argument list
 * will be passed as number 0, though.
 */

{
    svalue_t       *arg;    /* Pointer to efun arguments */
    int             delay;
    struct call    *cop;    /* New callout structure */
    struct call   **copp;   /* Auxiliary pointers for list insertion */
    struct call    *cop2;
    int             error_index;

    arg = sp - num_arg + 1;

    /* First, find a new call structure.
     * If possible from the free list, else allocate a new chunk of them.
     * Note that we don't yet remove the structure from the freelist - in
     * case errors happen.
     */

    if ( !(cop = call_list_free) )
    {
        int i;

        cop = call_list_free = pxalloc(CHUNK_SIZE * sizeof (struct call));
        for ( i = 0; i < CHUNK_SIZE - 1; i++)
            call_list_free[i].next = &call_list_free[i+1];
        call_list_free[CHUNK_SIZE-1].next = NULL;
        call_out_nil_object.flags |= O_DESTRUCTED;
        num_call += CHUNK_SIZE;
    }

    /* Test if the expected arguments are on the stack */

    if (arg[0].type != T_STRING && arg[0].type != T_CLOSURE)
    {
        bad_efun_vararg(1, sp);
        /* NOTREACHED */
    }

    if (arg[1].type != T_NUMBER)
    {
        bad_efun_vararg(2, sp);
        /* NOTREACHED */
    }

    /* If the current object is destructed, free everything on the stack
     * and return.
     */

    if (current_object->flags & O_DESTRUCTED)
    {
        do {
            free_svalue(sp--);
        } while (--num_arg);
        return sp;
    }

    /* Get the function designation from the stack */

    if (arg[0].type == T_STRING)
    {
        error_index = setup_function_callback(&(cop->fun), current_object
                                             , arg[0].u.string
                                             , num_arg-2, arg+2
                                             , MY_TRUE
                                             );
        free_string_svalue(arg);
    }
    else
        error_index = setup_closure_callback(&(cop->fun), arg
                                             , num_arg-2, arg+2
                                             , MY_TRUE
                                             );

    if (error_index >= 0)
    {
        /* call structure is still in the free list, and the
         * callback structure was invalidated automatically.
         */
        bad_efun_vararg(error_index, arg - 1);
        /* NOTREACHED */
    }

    /* We can do the callout, so lets remove it from the freelist and
     * store the missing data.
     */

    call_list_free = cop->next;
    cop->command_giver = command_giver; /* save current player context */
    if (command_giver)
        ref_object(command_giver, "new_call_out");  /* Bump its ref */

    /* Adjust the stack and get the delay */
    sp = arg - 1;
    delay = arg[1].u.number;
    if (delay < 1)
        delay = 1;

    /* Insert the new structure at its proper place in the list */

    for (copp = &call_list; NULL != (cop2 = *copp); copp = &cop2->next)
    {
        int delta;
        if ((delta = cop2->delta) >= delay)
        {
            cop2->delta -= delay;
            cop->delta = delay;
            cop->next = *copp;
            *copp = cop;
            return sp;
        }
        delay -= (delta >= 0 ? delta : 0);
          /* Especially when called from within a call_out, delta may be
           * negative.
           */
    }
    *copp = cop;
    cop->delta = delay;
    cop->next = NULL;

    return sp;
} /* new_call_out() */

/*-------------------------------------------------------------------------*/
void
call_out (void)

/* Check if there is any callout due to be called. If yes, do so.
 * This function is called from the heart_beat handling in the backend.c.
 * It sets up its own error recovery context so that errors during an
 * execution won't disturb the rest of the game.
 */

{
    static int last_time;
      /* Last time this function was called */

    static struct call *current_call_out;
      /* Current callout, static so that longjmp() won't clobber it. */

    static object_t *called_object;
      /* Object last called, static so that longjmp() won't clobber it */

    struct error_recovery_info error_recovery_info;

    /* No calls pending: just update the last_time and return */

    if (call_list == NULL)
    {
        last_time = current_time;
        return;
    }

    /* If not set yet, initialize last_time on the first call */
    if (last_time == 0)
        last_time = current_time;

    /* Update the first .delta in the list (so it won't happen
     * twice in case of an error.
     */
    call_list->delta -= current_time - last_time;

    last_time = current_time;
    current_interactive = NULL;

    /* Activate the local error recovery context */

    error_recovery_info.rt.last = rt_context;
    error_recovery_info.rt.type = ERROR_RECOVERY_BACKEND;
    rt_context = (rt_context_t *)&error_recovery_info;

    if (setjmp(error_recovery_info.con.text))
    {
        /* An error occured: recover and delete the guilty callout */

        struct call *cop;
        object_t *ob;
        wiz_list_t *user;

        clear_state();
        debug_message("%s: Error in call out.\n", time_stamp());
        cop = current_call_out;
        ob = called_object;
        if (ob)
        {
            user = ob->user;
            user->call_out_cost = eval_cost;
        }
        free_call(cop);
    }

    /* (Re)initialize stack and tracing */

    tracedepth = 0;

    /* Loop over the call list until it is empty or until all
     * due callouts are processed.
     */
    while (call_list && call_list->delta <= 0)
    {
        object_t    *ob;
        struct call *cop;
        wiz_list_t  *user;

        /* Move the first callout out of the chain.
         */
        cop = call_list;
        call_list = cop->next;
        current_call_out = cop;

        /* A special case:
         * If a lot of time has passed, so that current call out was missed,
         * then it will have a negative delta. This negative delta implies
         * that the next call out in the list has to be adjusted.
         */
        if (cop->delta < 0 && call_list)
            call_list->delta += cop->delta;


        /* Get the object for the function call and make sure it's valid */

        ob = callback_object(&(cop->fun));
        if (!ob)
        {
            /* Nothing to call */
            free_call(cop);
            continue;
        }

        if (O_PROG_SWAPPED(ob)
         && load_ob_from_swap(ob) < 0)
        {
            debug_message("%s: Error in call_out: out of memory: "
                          "unswap object '%s'.\n", time_stamp(), ob->name);
            free_call(cop);
            continue;
        }

        /* Determine the command_giver for the call.
         * If a command_giver is given in the callout structure, use that one.
         * Else test the object to be called or the object it's shadowing for
         * being a command_giver.
         * Remember that a now-destructed commandgiver is different from having
         * no commandgiver to begin with.
         */
        if (cop->command_giver)
        {
            if (!(cop->command_giver->flags & O_DESTRUCTED))
            {
                command_giver = cop->command_giver;
                if (O_IS_INTERACTIVE(command_giver))
                    trace_level = O_GET_INTERACTIVE(command_giver)->trace_level;
            }
            else
                command_giver = NULL;
        }
        else if (ob->flags & O_SHADOW)
        {
            /* Look at the object which is at the end of the shadow chain.
             */
            shadow_t *shadow_sent;
            object_t *sob;

            sob = ob;
            while ((shadow_sent = O_GET_SHADOW(sob)), shadow_sent->shadowing)
                sob = shadow_sent->shadowing;

            if (sob->flags & O_ENABLE_COMMANDS)
            {
                command_giver = sob;
                if (shadow_sent->ip)
                    trace_level = shadow_sent->ip->trace_level;
                else
                    trace_level = 0;
            }
            else
            {
                command_giver = NULL;
                trace_level = 0;
            }
        }
        else
        {
            /* If at all, this object must be the command_giver */

            if (ob->flags & O_ENABLE_COMMANDS)
                command_giver = ob;
            else
                command_giver = NULL;
            trace_level = 0;
        }

        /* Finally, call the function (unless the object was destructed).
         */

        called_object = current_object = ob;
        user = ob->user;
        if (user->last_call_out != current_time)
        {
            user->last_call_out = current_time;
            CLEAR_EVAL_COST;
        }
        else
            assigned_eval_cost = eval_cost = user->call_out_cost;

        (void)backend_callback(&(cop->fun), 0);
        user->call_out_cost = eval_cost;

        /* The function call used up all the arguments, now free
         * the rest
         */
        free_call(cop);

    } /* while (callouts pending) */

    rt_context = error_recovery_info.rt.last;
} /* call_out() */

/*-------------------------------------------------------------------------*/
void
find_call_out (object_t *ob, svalue_t *fun, Bool do_free_call)

/* Find the (first) callout for <ob>/<fun> (or <fun> if it is a closure).
 * If <do_free_call> is true, the found callout is removed.
 *
 * In either case, *<fun> is modified into a NUMBER holding the time left
 * for the found/removed callout. If no callout was found, -1 is returned.
 */

{
    struct call **copp, *cop;
    int delay = 0;
    char *fun_name;

    /* Find callout by closure */

    if (fun->type != T_STRING)
    {
        ph_int type;

        if (fun->type == T_CLOSURE)
        {
            if (!CLOSURE_MALLOCED(type = fun->x.closure_type)
             && type >= CLOSURE_EFUN)
            {
                for (copp = &call_list; NULL != (cop = *copp); copp = &cop->next)
                {
                    delay += cop->delta;
                    if (cop->fun.is_lambda
                     && cop->fun.function.lambda.x.closure_type == type
                     && cop->fun.function.lambda.u.ob == ob)
                    {
                        goto found;
                    }
                }
                goto not_found;
            }
            else if (type != CLOSURE_UNBOUND_LAMBDA)
            {
                lambda_t *l;

                l = fun->u.lambda;
                if (type != CLOSURE_LFUN)
                type = CLOSURE_UNBOUND_LAMBDA;
                for (copp = &call_list; NULL != (cop = *copp); copp = &cop->next)
                {
                    delay += cop->delta;
                    if (cop->fun.is_lambda
                     && (   cop->fun.function.lambda.u.lambda == l
                         || (   cop->fun.function.lambda.x.closure_type == type
                             && cop->fun.function.lambda.u.lambda->ob == l->ob
                             && cop->fun.function.lambda.u.lambda->function.index
                                == l->function.index)
                        )
                       )
                    {
                        goto found;
                    }
                }
                /* FALLTHROUGH*/
not_found:
                free_svalue(fun);
                put_number(fun, -1);
                return;
found:
                free_svalue(fun);
                if (do_free_call)
                {
                    if (cop->next)
                        cop->next->delta += cop->delta;
                    *copp = cop->next;
                    free_call(cop);
                }
                /* It is possible to have delay < 0 if we are
                 * called from inside call_out() .
                 */
                if (delay < 0)
                    delay = 0;
                put_number(fun, delay);
                return;
            }
        }
        bad_efun_arg(1, -1, fun);
        /* NOTREACHED */
    }


    /* Find callout by object/name */

    if (fun->x.string_type == STRING_SHARED) {
        fun_name = fun->u.string;
    }
    else
    {
        fun_name = make_shared_string(fun->u.string);
        if (fun->x.string_type == STRING_MALLOC)
            xfree(fun->u.string);
    }

    for (copp = &call_list; NULL != (cop = *copp); copp = &cop->next)
    {
        delay += cop->delta;
        if (cop->fun.function.named.ob == ob
         && cop->fun.function.named.name == fun_name
         && !cop->fun.is_lambda)
        {
            deref_string(fun_name);
            if (do_free_call)
            {
                if (cop->next)
                    cop->next->delta += cop->delta;
                *copp = cop->next;
                free_call(cop);
            }
            if (delay < 0)
                delay = 0;
            put_number(fun, delay);
            return;
        }
    }
    free_string(fun_name);
    put_number(fun, -1);
}

/*-------------------------------------------------------------------------*/
size_t
print_call_out_usage (strbuf_t *sbuf, Bool verbose)

/* Compute and return the amount of memory used by callouts.
 * If <verbose> is true, write detailed statistics to the current user.
 */

{
    long i;
    struct call *cop;

    for (i=0, cop = call_list; cop; cop = cop->next)
        i++;
    if (verbose)
    {
        strbuf_add(sbuf, "\nCall out information:\n");
        strbuf_add(sbuf,"---------------------\n");
        strbuf_addf(sbuf, "Number of allocated call outs: %8ld, %8ld bytes\n",
                    num_call, num_call * sizeof (struct call));
        strbuf_addf(sbuf, "Current length: %ld\n", i);
    }
    else
    {
        strbuf_addf(sbuf, "call out:\t\t\t%8ld %8ld (current length %ld)\n"
                   , num_call, num_call * sizeof (struct call), i);
    }

    return num_call * sizeof (struct call);
}

/*-------------------------------------------------------------------------*/
void
callout_dinfo_status (svalue_t *svp)

/* Return the callout information for debug_info(DINFO_DATA, DID_STATUS).
 * <svp> points to the svalue block for the result, this function fills in
 * the spots for the object table.
 */

{
    long i;
    struct call *cop;
    
    for (i=0, cop = call_list; cop; cop = cop->next)
        i++;

    svp[DID_ST_CALLOUTS].u.number      = i;
    svp[DID_ST_CALLOUT_SLOTS].u.number = num_call;
    svp[DID_ST_CALLOUT_SIZE].u.number  = num_call * sizeof(struct call);
} /* callout_dinfo_status() */

/*-------------------------------------------------------------------------*/
#ifdef DEBUG

void
count_extra_ref_from_call_outs (void)

/* Used to debug refcounts: count all refcounts in the callout handling.
 */

{
    struct call *cop;

    for (cop = call_list; cop; cop = cop->next)
    {
        count_callback_extra_refs(&(cop->fun));
        if (cop->command_giver)
            count_extra_ref_in_object(cop->command_giver);
    }
}

#endif

/*-------------------------------------------------------------------------*/
void
remove_stale_call_outs (void)

/* GC Support: Remove all callouts referencing destructed objects.
 */

{
    struct call **copp, *cop;

    for (copp = &call_list; NULL != (cop = *copp); )
    {
        object_t *ob;

        ob = callback_object(&(cop->fun));
        if (!ob)
        {
            if (cop->next)
                cop->next->delta += cop->delta;
            *copp = cop->next;
            free_call(cop);
            continue;
        }
        copp = &cop->next;
    }
} /* remove_stale_call_outs() */


#ifdef GC_SUPPORT

/*-------------------------------------------------------------------------*/
void
clear_ref_from_call_outs (void)

/* GC Support: Clear all refs from the callout handling.
 */

{
    struct call *cop;

    for (cop = call_list; cop; cop = cop->next)
    {
        object_t *ob;

        clear_ref_in_callback(&(cop->fun));

        if (NULL != (ob = cop->command_giver) && ob->flags & O_DESTRUCTED)
        {
            ob->ref = 0;
            clear_inherit_ref(ob->prog);
        }
    }
} /* clear_ref_from_call_outs() */

/*-------------------------------------------------------------------------*/
void
count_ref_from_call_outs (void)

/* GC Support: Clear all refs from the callout handling.
 */

{
    struct call *cop;
    object_t *ob;

    for (cop = call_list; cop; cop = cop->next)
    {
        count_ref_in_callback(&(cop->fun));

        if ( NULL != (ob = cop->command_giver) )
        {
            if (ob->flags & O_DESTRUCTED) {
                reference_destructed_object(ob);
                cop->command_giver = NULL;
            } else {
                ob->ref++;
            }
        }
    }
}

#endif /* GC_SUPPORT */

/*-------------------------------------------------------------------------*/
vector_t *
get_all_call_outs (void)

/* Construct an array of all pending call_outs (whose object is not
 * destructed). Every item in the array is itself an array of 4 or
 * more entries:
 *  0:   The object (only if the function is a string).
 *  1:   The function (string or closure).
 *  2:   The delay.
 *  3..: The argument(s).
 */
{
    int i, next_time;
    struct call *cop;
    vector_t *v;

    /* Count the number of pending callouts and allocate
     * the result array.
     */
    for (i = 0, cop = call_list; cop; cop = cop->next)
    {
        if (!callback_object(&(cop->fun)))
            continue;
        i++;
    }
    v = allocate_array(i); /* assume that all elements are inited to 0 */

    /* Create the result array contents.
     */

    next_time = 0;
    for (i=0, cop = call_list; cop; cop = cop->next)
    {
        vector_t *vv;
        object_t *ob;

        next_time += cop->delta;

        ob = callback_object(&(cop->fun));
        if (!ob)
            continue;
            
        /* Get the subarray */

        vv = allocate_array(3 + cop->fun.num_arg);

        if (cop->fun.is_lambda)
        {
            assign_svalue_no_free(&vv->item[1], &cop->fun.function.lambda);
            /* assuming that item[0] was inited to 0 */
        }
        else
        {
            put_ref_object(vv->item, ob, "get_all_call_outs");
            put_ref_string(vv->item + 1, cop->fun.function.named.name);
        }

        vv->item[2].u.number = next_time;

        if (cop->fun.num_arg > 0)
        {
            svalue_t *source, *dest;
            int nargs;

            nargs = cop->fun.num_arg;
            if (nargs > 1)
                source = cop->fun.arg.u.lvalue;
            else
                source = &(cop->fun.arg);
            dest = &vv->item[3];
            do {
                assign_svalue_no_free(dest++, source++);
            } while (--nargs);
        }

        put_array(v->item + i, vv);
        i++;
    }

    return v;
}

/***************************************************************************/

