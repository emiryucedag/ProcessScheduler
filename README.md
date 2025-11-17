# Process Scheduler

This project implements a Priority-SRTF based non-preemptive process scheduler in C. It uses the pthread library to manage I/O operations in a separate thread.

## Project Features

Scheduler: Selects processes based on Priority (0 is highest). If priorities are equal, it uses SRTF (Shortest Remaining Time First).

Non-Preemptive: The currently running process is not interrupted until it finishes its CPU burst or terminates.

Aging: Processes waiting in the ready queue for 100ms have their priority value decremented by 1 to prevent starvation.

Concurrency: A dedicated thread handles processes in the waiting queue (I/O).

## Compilation

To compile the project, run:

make


## Usage

Run the scheduler with an input file:

./process_scheduler <input_file>


## Example:

./process_scheduler processes.txt


## Input Format

The input file should contain process details in the following order:
[PID] [Arrival Time] [Total CPU Time] [Interval Time] [I/O Time] [Priority]

## Cleaning

To remove compiled files:

make clean
