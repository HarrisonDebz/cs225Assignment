#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROCESSES 20
#define MAX_NAME_LEN 50
#define RR_TIME_QUANTUM 3

typedef enum {
  NEW = 0,
  READY = 1,
  RUNNING = 2,
  WAITING = 3,
  TERMINATED = 4
} ProcessState;
typedef enum { AMBULANCE = 1, FIRE = 2, POLICE = 3 } EmergencyType;

typedef struct {
  int pid;
  char name[MAX_NAME_LEN];
  EmergencyType emergency_type;
  ProcessState state;
  int arrival_time, burst_time, remaining_time, priority;
  int start_time, finish_time, waiting_time, turnaround_time, response_time;
} PCB;

typedef struct QueueNode {
  PCB *process;
  struct QueueNode *next;
} QueueNode;
typedef struct {
  QueueNode *front, *rear;
  int size;
} ReadyQueue;
typedef struct {
  double avg_waiting_time, avg_turnaround_time, avg_response_time,
      cpu_utilization;
  int total_time, n_processes;
} SchedulingResult;
typedef struct {
  int pid;
  char name[MAX_NAME_LEN];
  int start, end;
} GanttEntry;

/* ===========================================================
 *  SECTION 1 — HELPER / UTILITY FUNCTIONS
 * =========================================================== */

/* Convert ProcessState enum to a readable string */
const char *state_to_str(ProcessState s) {
  switch (s) {
  case NEW:
    return "NEW";
  case READY:
    return "READY";
  case RUNNING:
    return "RUNNING";
  case WAITING:
    return "WAITING";
  case TERMINATED:
    return "TERMINATED";
  default:
    return "UNKNOWN";
  }
}

/* Convert EmergencyType enum to a readable string */
const char *emergency_to_str(EmergencyType t) {
  switch (t) {
  case AMBULANCE:
    return "Ambulance";
  case FIRE:
    return "Fire";
  case POLICE:
    return "Police";
  default:
    return "Unknown";
  }
}

/* Build and return a new PCB */
PCB create_process(int pid, const char *name, EmergencyType type, int arrival,
                   int burst, int priority) {
  PCB p;
  p.pid = pid;
  strncpy(p.name, name, MAX_NAME_LEN - 1);
  p.name[MAX_NAME_LEN - 1] = '\0';
  p.emergency_type = type;
  p.state = NEW;
  p.arrival_time = arrival;
  p.burst_time = burst;
  p.remaining_time = burst; /* initialised to full burst for RR */
  p.priority = priority;
  /* Metrics start at -1 (not yet computed) */
  p.start_time = -1;
  p.finish_time = -1;
  p.waiting_time = -1;
  p.turnaround_time = -1;
  p.response_time = -1;
  return p;
}

/* Print a single PCB summary */
void print_pcb(PCB *p) {
  printf("  PID %-3d | %-28s | %-9s | Priority %-2d | "
         "Arrival %-3d | Burst %-3d | State: %s\n",
         p->pid, p->name, emergency_to_str(p->emergency_type), p->priority,
         p->arrival_time, p->burst_time, state_to_str(p->state));
}

/* --- Log stub -----------------------------------------------
 * Integrate with Part 4's file management module.
 * Replace body with a call to that module's log function.       */
static void log_event(const char *event) {
  FILE *fp = fopen("serc_scheduler.log", "a");
  if (fp) {
    fprintf(fp, "[SCHEDULER] %s\n", event);
    fclose(fp);
  }
}

/* ===========================================================
 *  SECTION 2 — READY QUEUE (FIFO LINKED LIST)
 * =========================================================== */

ReadyQueue *create_queue(void) {
  ReadyQueue *q = (ReadyQueue *)malloc(sizeof(ReadyQueue));
  if (!q) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  q->front = q->rear = NULL;
  q->size = 0;
  return q;
}

void enqueue(ReadyQueue *q, PCB *p) {
  QueueNode *node = (QueueNode *)malloc(sizeof(QueueNode));
  if (!node) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  node->process = p;
  node->next = NULL;
  if (q->rear)
    q->rear->next = node;
  else
    q->front = node;
  q->rear = node;
  q->size++;
}

PCB *dequeue(ReadyQueue *q) {
  if (!q->front)
    return NULL;
  QueueNode *temp = q->front;
  PCB *p = temp->process;
  q->front = q->front->next;
  if (!q->front)
    q->rear = NULL;
  free(temp);
  q->size--;
  return p;
}

PCB *peek(ReadyQueue *q) { return q->front ? q->front->process : NULL; }

int is_empty(ReadyQueue *q) { return q->size == 0; }

void free_queue(ReadyQueue *q) {
  while (!is_empty(q))
    dequeue(q);
  free(q);
}

/* ===========================================================
 *  SECTION 3 — ALGORITHM 1: ROUND ROBIN (PREEMPTIVE)
 *
 *  Approach:
 *    - Processes are admitted to the ready queue as they arrive.
 *    - The CPU serves the front of the queue for at most `quantum`
 *      time units before preempting and cycling to the next process.
 *    - Newly arrived processes are added BEFORE re-queuing the
 *      preempted process (standard RR convention).
 *    - All five process states are updated during simulation.
 * =========================================================== */

SchedulingResult run_round_robin(PCB processes[], int n, int quantum,
                                 GanttEntry gantt[], int *gantt_len) {
  /* Working copies so the originals stay clean for comparison */
  PCB work[MAX_PROCESSES];
  for (int i = 0; i < n; i++) {
    work[i] = processes[i];
    work[i].state = NEW;
    work[i].remaining_time = processes[i].burst_time;
    work[i].start_time = -1;
  }

  ReadyQueue *queue = create_queue();
  int time = 0; /* current clock tick */
  int done = 0; /* number of completed processes */
  int gidx = 0; /* Gantt chart index */
  int total_burst = 0;

  for (int i = 0; i < n; i++)
    total_burst += work[i].burst_time;

  char log_buf[128];
  snprintf(log_buf, sizeof(log_buf), "Round Robin started. n=%d, quantum=%d", n,
           quantum);
  log_event(log_buf);

  /* Seed queue with processes that arrive at time 0 */
  for (int i = 0; i < n; i++) {
    if (work[i].arrival_time == 0) {
      work[i].state = READY;
      enqueue(queue, &work[i]);
    }
  }

  while (done < n) {
    if (is_empty(queue)) {
      /* CPU idle – advance to next arrival */
      int next_arrival = __INT_MAX__;
      for (int i = 0; i < n; i++) {
        if (work[i].state != TERMINATED && work[i].arrival_time > time)
          next_arrival = (work[i].arrival_time < next_arrival)
                             ? work[i].arrival_time
                             : next_arrival;
      }
      /* Record idle slot in Gantt */
      gantt[gidx].pid = -1;
      strcpy(gantt[gidx].name, "IDLE");
      gantt[gidx].start = time;
      time = next_arrival;
      gantt[gidx].end = time;
      gidx++;

      /* Admit newly arrived processes */
      for (int i = 0; i < n; i++) {
        if (work[i].state == NEW && work[i].arrival_time <= time) {
          work[i].state = READY;
          enqueue(queue, &work[i]);
        }
      }
      continue;
    }

    PCB *curr = dequeue(queue);
    curr->state = RUNNING;

    /* Record first-run time */
    if (curr->start_time == -1)
      curr->start_time = time;

    /* Determine how long this slice runs */
    int slice =
        (curr->remaining_time < quantum) ? curr->remaining_time : quantum;

    /* Gantt entry */
    gantt[gidx].pid = curr->pid;
    strncpy(gantt[gidx].name, curr->name, MAX_NAME_LEN - 1);
    gantt[gidx].start = time;
    gantt[gidx].end = time + slice;
    gidx++;

    /* Advance time and reduce remaining burst */
    int end_slice = time + slice;
    curr->remaining_time -= slice;

    /* Admit processes that arrived during this slice */
    for (int i = 0; i < n; i++) {
      if (work[i].state == NEW && work[i].arrival_time > time &&
          work[i].arrival_time <= end_slice) {
        work[i].state = READY;
        enqueue(queue, &work[i]);
      }
    }

    time = end_slice;

    if (curr->remaining_time == 0) {
      /* Process finished */
      curr->state = TERMINATED;
      curr->finish_time = time;
      curr->turnaround_time = curr->finish_time - curr->arrival_time;
      curr->waiting_time = curr->turnaround_time - curr->burst_time;
      curr->response_time = curr->start_time - curr->arrival_time;
      done++;

      snprintf(log_buf, sizeof(log_buf),
               "PID %d (%s) TERMINATED at t=%d | WT=%d TAT=%d", curr->pid,
               curr->name, time, curr->waiting_time, curr->turnaround_time);
      log_event(log_buf);
    } else {
      /* Preempted — put back in queue */
      curr->state = READY;
      enqueue(queue, curr);
    }
  }

  *gantt_len = gidx;
  free_queue(queue);

  /* Copy metrics back to original array */
  for (int i = 0; i < n; i++) {
    processes[i].start_time = work[i].start_time;
    processes[i].finish_time = work[i].finish_time;
    processes[i].waiting_time = work[i].waiting_time;
    processes[i].turnaround_time = work[i].turnaround_time;
    processes[i].response_time = work[i].response_time;
    processes[i].state = TERMINATED;
  }

  /* Compute aggregate results */
  SchedulingResult r = {0};
  r.n_processes = n;
  r.total_time = time;
  for (int i = 0; i < n; i++) {
    r.avg_waiting_time += processes[i].waiting_time;
    r.avg_turnaround_time += processes[i].turnaround_time;
    r.avg_response_time += processes[i].response_time;
  }
  r.avg_waiting_time /= n;
  r.avg_turnaround_time /= n;
  r.avg_response_time /= n;
  r.cpu_utilization = ((double)total_burst / time) * 100.0;

  return r;
}

/* ===========================================================
 *  SECTION 4 — ALGORITHM 2: PRIORITY SCHEDULING (NON-PREEMPTIVE)
 *
 *  Approach:
 *    - At each scheduling point the ready process with the
 *      LOWEST priority number (= most urgent) is selected.
 *    - Ties are broken by arrival time (earlier wins), which
 *      prevents indefinite starvation of equal-priority tasks.
 *    - Emergency type directly drives priority:
 *        Ambulance (1) > Fire (2) > Police (3)
 * =========================================================== */

SchedulingResult run_priority_scheduling(PCB processes[], int n,
                                         GanttEntry gantt[], int *gantt_len) {
  PCB work[MAX_PROCESSES];
  int completed[MAX_PROCESSES];

  for (int i = 0; i < n; i++) {
    work[i] = processes[i];
    work[i].state = NEW;
    completed[i] = 0;
  }

  int time = 0;
  int done = 0;
  int gidx = 0;
  int total_burst = 0;

  for (int i = 0; i < n; i++)
    total_burst += work[i].burst_time;

  char log_buf[128];
  log_event("Priority Scheduling (non-preemptive) started.");

  while (done < n) {
    /* -- Find highest-priority READY process -- */
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (completed[i])
        continue;
      if (work[i].arrival_time > time)
        continue;

      if (best == -1 || work[i].priority < work[best].priority ||
          (work[i].priority == work[best].priority &&
           work[i].arrival_time < work[best].arrival_time)) {
        best = i;
      }
    }

    if (best == -1) {
      /* No ready process – CPU idle */
      int next_arrival = __INT_MAX__;
      for (int i = 0; i < n; i++) {
        if (!completed[i] && work[i].arrival_time < next_arrival)
          next_arrival = work[i].arrival_time;
      }
      gantt[gidx].pid = -1;
      strcpy(gantt[gidx].name, "IDLE");
      gantt[gidx].start = time;
      time = next_arrival;
      gantt[gidx].end = time;
      gidx++;
      continue;
    }

    PCB *curr = &work[best];
    curr->state = RUNNING;

    /* First-run time */
    if (curr->start_time == -1)
      curr->start_time = time;

    /* Gantt entry – runs to completion (non-preemptive) */
    gantt[gidx].pid = curr->pid;
    strncpy(gantt[gidx].name, curr->name, MAX_NAME_LEN - 1);
    gantt[gidx].start = time;
    gantt[gidx].end = time + curr->burst_time;
    gidx++;

    time += curr->burst_time;

    curr->state = TERMINATED;
    curr->finish_time = time;
    curr->turnaround_time = curr->finish_time - curr->arrival_time;
    curr->waiting_time = curr->turnaround_time - curr->burst_time;
    curr->response_time = curr->start_time - curr->arrival_time;
    completed[best] = 1;
    done++;

    snprintf(log_buf, sizeof(log_buf),
             "PID %d (%s) TERMINATED at t=%d | Priority=%d WT=%d TAT=%d",
             curr->pid, curr->name, time, curr->priority, curr->waiting_time,
             curr->turnaround_time);
    log_event(log_buf);
  }

  *gantt_len = gidx;

  /* Copy metrics back */
  for (int i = 0; i < n; i++) {
    processes[i].start_time = work[i].start_time;
    processes[i].finish_time = work[i].finish_time;
    processes[i].waiting_time = work[i].waiting_time;
    processes[i].turnaround_time = work[i].turnaround_time;
    processes[i].response_time = work[i].response_time;
    processes[i].state = TERMINATED;
  }

  SchedulingResult r = {0};
  r.n_processes = n;
  r.total_time = time;
  for (int i = 0; i < n; i++) {
    r.avg_waiting_time += processes[i].waiting_time;
    r.avg_turnaround_time += processes[i].turnaround_time;
    r.avg_response_time += processes[i].response_time;
  }
  r.avg_waiting_time /= n;
  r.avg_turnaround_time /= n;
  r.avg_response_time /= n;
  r.cpu_utilization = ((double)total_burst / time) * 100.0;

  return r;
}

/* ===========================================================
 *  SECTION 5 — DISPLAY FUNCTIONS
 * =========================================================== */

/* Renders a text-based Gantt chart to stdout */
void display_gantt_chart(GanttEntry gantt[], int len) {
  printf("\n  +==================================================+\n");
  printf("  |               GANTT CHART                       |\n");
  printf("  +==================================================+\n");

  /* Top border */
  printf("  ");
  for (int i = 0; i < len; i++) {
    int width = gantt[i].end - gantt[i].start;
    for (int w = 0; w < width * 2 + 1; w++)
      printf("-");
  }
  printf("\n  |");

  /* Process labels */
  for (int i = 0; i < len; i++) {
    int width = gantt[i].end - gantt[i].start;
    if (gantt[i].pid == -1) {
      printf(" IDLE");
      for (int w = 0; w < width * 2 - 4; w++)
        printf(" ");
    } else {
      printf(" P%-2d", gantt[i].pid);
      for (int w = 0; w < width * 2 - 3; w++)
        printf(" ");
    }
    printf("|");
  }

  /* Bottom border */
  printf("\n  ");
  for (int i = 0; i < len; i++) {
    int width = gantt[i].end - gantt[i].start;
    for (int w = 0; w < width * 2 + 1; w++)
      printf("-");
  }

  /* Time axis */
  printf("\n  ");
  int prev = -1;
  for (int i = 0; i < len; i++) {
    int width = gantt[i].end - gantt[i].start;
    if (gantt[i].start != prev) {
      printf("%-*d", width * 2, gantt[i].start);
    } else {
      printf("%-*s", width * 2, "");
    }
    prev = gantt[i].start;
  }
  if (len > 0)
    printf("%d", gantt[len - 1].end);
  printf("\n\n");
}

/* Per-process metrics table */
void display_metrics_table(PCB processes[], int n) {
  printf("\n  "
         "+===================================================================="
         "==+\n");
  printf("  |                     PROCESS SCHEDULING METRICS                   "
         "   |\n");
  printf("  "
         "+======╦======================╦====╦====╦========╦==========╦========"
         "+\n");
  printf("  | PID  | Name                 | AT | BT |  Start |  Finish  |   WT "
         "  |\n");
  printf("  |      |                      |    |    |        |          | TAT "
         "RT |\n");
  printf("  "
         "+======+======================+====+====+========+==========+========"
         "+\n");

  for (int i = 0; i < n; i++) {
    PCB *p = &processes[i];
    printf("  | P%-3d | %-20s | %-2d | %-2d |  %-5d |   %-5d  |WT=%-2d   |\n",
           p->pid, p->name, p->arrival_time, p->burst_time, p->start_time,
           p->finish_time, p->waiting_time);
    printf("  |      |                      |    |    |        |          "
           "|TAT=%-2d |\n",
           p->turnaround_time);
    printf("  |      |                      |    |    |        |          "
           "|RT=%-2d  |\n",
           p->response_time);
    printf("  "
           "+======+======================+====+====+========+==========+======"
           "==+\n");
  }
  printf("  | AT=Arrival Time  BT=Burst Time  WT=Waiting  TAT=Turnaround  "
         "RT=Response |\n");
  printf("  "
         "+===================================================================="
         "=======+\n");
}

/* Summary results box */
void display_aggregate_results(SchedulingResult *r, const char *algo_name) {
  printf("\n  +==================================================+\n");
  printf("  |    AGGREGATE RESULTS — %-24s|\n", algo_name);
  printf("  +==================================================+\n");
  printf("  |  Processes Scheduled : %-26d|\n", r->n_processes);
  printf("  |  Total Makespan      : %-24d ms|\n", r->total_time);
  printf("  |  Avg Waiting Time    : %-23.2f ms|\n", r->avg_waiting_time);
  printf("  |  Avg Turnaround Time : %-23.2f ms|\n", r->avg_turnaround_time);
  printf("  |  Avg Response Time   : %-23.2f ms|\n", r->avg_response_time);
  printf("  |  CPU Utilization     : %-22.1f %%  |\n", r->cpu_utilization);
  printf("  +==================================================+\n\n");
}

/* ===========================================================
 *  SECTION 6 — INTERACTIVE MENU (CPU SCHEDULING MODULE)
 *
 *  Call cpu_scheduling_module() from your main() or the
 *  Part 4 CLI dispatcher.
 * =========================================================== */

/* Prompt user to build the process table interactively */
static int get_processes_from_user(PCB processes[]) {
  int n;
  printf("\n  How many emergency tasks to schedule? (1-%d): ", MAX_PROCESSES);
  scanf("%d", &n);
  if (n < 1 || n > MAX_PROCESSES) {
    printf("  Invalid number. Returning.\n");
    return 0;
  }

  for (int i = 0; i < n; i++) {
    int arrival, burst, etype;
    printf("\n  -- Task %d ------------------------------\n", i + 1);
    printf("  Emergency type  [1=Ambulance 2=Fire 3=Police]: ");
    scanf("%d", &etype);
    if (etype < 1 || etype > 3)
      etype = 3;

    printf("  Arrival time (ms): ");
    scanf("%d", &arrival);
    printf("  Burst time   (ms): ");
    scanf("%d", &burst);
    if (burst < 1)
      burst = 1;

    /* Priority defaults to emergency type; user can override */
    int priority = etype;
    printf("  Priority [default=%d, lower=more urgent]: ", priority);
    char buf[16];
    /* flush then read optional override */
    while (getchar() != '\n')
      ;
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n')
      priority = atoi(buf);
    if (priority < 1)
      priority = 1;

    /* Auto-generate a descriptive name */
    char name[MAX_NAME_LEN];
    const char *type_names[] = {"", "Ambulance", "Fire", "Police"};
    snprintf(name, MAX_NAME_LEN, "%s_Task_%d", type_names[etype], i + 1);

    processes[i] = create_process(i + 1, name, (EmergencyType)etype, arrival,
                                  burst, priority);
    printf("  [x] Created: ");
    print_pcb(&processes[i]);
  }
  return n;
}

/* Load a built-in demo scenario for quick testing */
static int load_demo_scenario(PCB processes[]) {
  /*
   * Demo scenario: city multi-emergency incident.
   *
   *  PID  Name                       Type       AT  BT  Pri
   *   1   Ambulance_Task_1           Ambulance   0   8    1
   *   2   Fire_Task_2                Fire        1   4    2
   *   3   Police_Task_3              Police      2   9    3
   *   4   Ambulance_Task_4           Ambulance   3   5    1
   *   5   Fire_Task_5                Fire        4   2    2
   */
  processes[0] = create_process(1, "Ambulance_Task_1", AMBULANCE, 0, 8, 1);
  processes[1] = create_process(2, "Fire_Task_2", FIRE, 1, 4, 2);
  processes[2] = create_process(3, "Police_Task_3", POLICE, 2, 9, 3);
  processes[3] = create_process(4, "Ambulance_Task_4", AMBULANCE, 3, 5, 1);
  processes[4] = create_process(5, "Fire_Task_5", FIRE, 4, 2, 2);
  return 5;
}

/* Main entry point for the CPU scheduling sub-module */
void cpu_scheduling_module(void) {
  PCB original[MAX_PROCESSES];
  PCB processes[MAX_PROCESSES];
  GanttEntry gantt[MAX_PROCESSES * 20]; /* RR can create many slices */
  int n = 0, gantt_len = 0;
  int quantum = RR_TIME_QUANTUM;
  int choice;

  printf("\n");
  printf("  **************************************************\n");
  printf("  *   SERC MINI-OS  —  CPU SCHEDULING MODULE       *\n");
  printf("  **************************************************\n");

  /* -- Step 1: Load processes -- */
  printf("\n  [1] Enter tasks manually\n");
  printf("  [2] Load demo scenario (5 emergency tasks)\n");
  printf("  Choice: ");
  scanf("%d", &choice);

  if (choice == 1) {
    n = get_processes_from_user(original);
    if (n == 0)
      return;
  } else {
    n = load_demo_scenario(original);
    printf("\n  Demo scenario loaded (%d tasks).\n", n);
    for (int i = 0; i < n; i++)
      print_pcb(&original[i]);
  }

  /* -- Step 2: Algorithm selection -- */
  int running = 1;
  while (running) {
    printf("\n  +-----------------------------------------+\n");
    printf("  |       SELECT SCHEDULING ALGORITHM       |\n");
    printf("  +-----------------------------------------+\n");
    printf("  |  [1] Round Robin (RR)                   |\n");
    printf("  |  [2] Priority Scheduling (PS)           |\n");
    printf("  |  [3] Run BOTH and compare               |\n");
    printf("  |  [4] Change time quantum (current: %2d)  |\n", quantum);
    printf("  |  [5] Reload / new task set              |\n");
    printf("  |  [0] Return to main menu                |\n");
    printf("  +-----------------------------------------+\n");
    printf("  Choice: ");
    scanf("%d", &choice);

    switch (choice) {

    /* -- Round Robin -- */
    case 1: {
      memcpy(processes, original, sizeof(PCB) * n);
      printf("\n  Running Round Robin (quantum = %d ms)...\n", quantum);
      SchedulingResult rr =
          run_round_robin(processes, n, quantum, gantt, &gantt_len);
      display_gantt_chart(gantt, gantt_len);
      display_metrics_table(processes, n);
      display_aggregate_results(&rr, "Round Robin");
      break;
    }

    /* -- Priority Scheduling -- */
    case 2: {
      memcpy(processes, original, sizeof(PCB) * n);
      printf("\n  Running Priority Scheduling...\n");
      SchedulingResult ps =
          run_priority_scheduling(processes, n, gantt, &gantt_len);
      display_gantt_chart(gantt, gantt_len);
      display_metrics_table(processes, n);
      display_aggregate_results(&ps, "Priority Scheduling");
      break;
    }

    /* -- Compare Both -- */
    case 3: {
      PCB proc_rr[MAX_PROCESSES], proc_ps[MAX_PROCESSES];
      GanttEntry g_rr[MAX_PROCESSES * 20], g_ps[MAX_PROCESSES];
      int glen_rr = 0, glen_ps = 0;

      memcpy(proc_rr, original, sizeof(PCB) * n);
      memcpy(proc_ps, original, sizeof(PCB) * n);

      printf("\n  ========== ROUND ROBIN ==========\n");
      SchedulingResult rr =
          run_round_robin(proc_rr, n, quantum, g_rr, &glen_rr);
      display_gantt_chart(g_rr, glen_rr);
      display_metrics_table(proc_rr, n);
      display_aggregate_results(&rr, "Round Robin");

      printf("\n  ========== PRIORITY SCHEDULING ==========\n");
      SchedulingResult ps = run_priority_scheduling(proc_ps, n, g_ps, &glen_ps);
      display_gantt_chart(g_ps, glen_ps);
      display_metrics_table(proc_ps, n);
      display_aggregate_results(&ps, "Priority Scheduling");

      /* -- Side-by-side comparison -- */
      printf(
          "  +==========================================================+\n");
      printf("  |                ALGORITHM COMPARISON                     |\n");
      printf("  +==========================╦=============╦================+\n");
      printf("  | Metric                   | Round Robin |    Priority    |\n");
      printf("  +==========================+=============+================+\n");
      printf("  | Avg Waiting Time (ms)    | %11.2f | %14.2f |\n",
             rr.avg_waiting_time, ps.avg_waiting_time);
      printf("  | Avg Turnaround Time (ms) | %11.2f | %14.2f |\n",
             rr.avg_turnaround_time, ps.avg_turnaround_time);
      printf("  | Avg Response Time (ms)   | %11.2f | %14.2f |\n",
             rr.avg_response_time, ps.avg_response_time);
      printf("  | CPU Utilization (%%)      | %10.1f%% | %13.1f%% |\n",
             rr.cpu_utilization, ps.cpu_utilization);
      printf("  +==========================+=============+================+\n");

      /* Declare winner per metric */
      printf("  | Better Avg Waiting       | %11s | %14s |\n",
             rr.avg_waiting_time <= ps.avg_waiting_time ? "[x]" : "",
             ps.avg_waiting_time < rr.avg_waiting_time ? "[x]" : "");
      printf("  | Better Avg Turnaround    | %11s | %14s |\n",
             rr.avg_turnaround_time <= ps.avg_turnaround_time ? "[x]" : "",
             ps.avg_turnaround_time < rr.avg_turnaround_time ? "[x]" : "");
      printf("  | Better CPU Utilization   | %11s | %14s |\n",
             rr.cpu_utilization >= ps.cpu_utilization ? "[x]" : "",
             ps.cpu_utilization > rr.cpu_utilization ? "[x]" : "");
      printf("  +==========================╩=============╩================+\n");
      printf(
          "\n  NOTE: Round Robin ensures fairness and responsiveness (good\n");
      printf(
          "        for equal-priority tasks). Priority Scheduling minimises\n");
      printf("        turnaround for the highest-urgency emergencies "
             "(Ambulance\n");
      printf("        > Fire > Police) but may starve low-priority "
             "processes.\n\n");
      break;
    }

    /* -- Change quantum -- */
    case 4:
      printf("  New time quantum (ms): ");
      scanf("%d", &quantum);
      if (quantum < 1)
        quantum = 1;
      printf("  Time quantum set to %d ms.\n", quantum);
      break;

    /* -- Reload processes -- */
    case 5:
      printf("  [1] Enter tasks manually  [2] Demo scenario: ");
      scanf("%d", &choice);
      if (choice == 1)
        n = get_processes_from_user(original);
      else
        n = load_demo_scenario(original);
      break;

    case 0:
      running = 0;
      break;

    default:
      printf("  Invalid option.\n");
    }
  }
}

/* ===========================================================
 *  SECTION 7 — STANDALONE MAIN (remove when integrating)
 *
 *  Compile standalone:
 *    gcc -Wall -o cpu_scheduler cpu_scheduler.c && ./cpu_scheduler
 *
 *  Compile with teammates' modules:
 *    gcc -Wall -o serc_os main.c cpu_scheduler.c pcb.c memory.c ipc.c cli.c
 * =========================================================== */

int main(void) {
  cpu_scheduling_module();
  return 0;
}